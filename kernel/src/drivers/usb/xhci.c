#include "usb.h"
#include "usb_kbd.h"
#include "../pci.h"
#include "../serial.h"
#include "../timer.h"
#include "../../boot/requests.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"

#include <stddef.h>
#include <stdint.h>

/* The xHCI spec (eXtensible Host Controller Interface, rev 1.2) is the
 * reference for every register, bit and structure below; section
 * numbers are given where it helps. */

#define MAX_CONTROLLERS 4
#define RING_TRBS (PMM_PAGE_SIZE / 16) /* One page per ring, last TRB is the link. */

/* Capability registers (5.3). */
#define CAP_CAPLENGTH 0x00
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF 0x14
#define CAP_RTSOFF 0x18

/* Operational registers (5.4). */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_CRCR 0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC(i) (0x400 + 0x10 * (i))

#define USBCMD_RUN (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH (1u << 0)
#define USBSTS_CNR (1u << 11)

#define PORTSC_CCS (1u << 0)
#define PORTSC_PED (1u << 1)
#define PORTSC_PR (1u << 4)
#define PORTSC_PP (1u << 9)
#define PORTSC_LWS (1u << 16)
#define PORTSC_PRC (1u << 21)
#define PORTSC_CHANGE_BITS 0x00FE0000u /* CSC..CEC, all write-1-to-clear. */
/* What to write back so nothing changes by accident: PED is
 * write-1-to-disable, LWS would strobe a link state change, and the
 * change bits are write-1-to-clear. */
#define PORTSC_KEEP(v) ((v) & ~(PORTSC_PED | PORTSC_LWS | PORTSC_CHANGE_BITS))

/* Interrupter 0, in the runtime registers (5.5.2). */
#define IR0_IMAN 0x20
#define IR0_ERSTSZ 0x28
#define IR0_ERSTBA 0x30
#define IR0_ERDP 0x38
#define ERDP_EHB (1u << 3)

/* TRBs (6.4). */
struct xhci_trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};

#define TRB_CYCLE (1u << 0)
#define TRB_TOGGLE_CYCLE (1u << 1) /* Link TRBs. */
#define TRB_ISP (1u << 2)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_DIR_IN (1u << 16)
#define TRB_TYPE(t) ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3F)

#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT 13
#define TRB_DISABLE_SLOT 10
#define TRB_RESET_ENDPOINT 14
#define TRB_SET_TR_DEQUEUE 16
#define TRB_EVENT_TRANSFER 32
#define TRB_EVENT_COMMAND 33
#define TRB_EVENT_PORT_STATUS 34

#define CC_SUCCESS 1
#define CC_STALL 6
#define CC_SHORT_PACKET 13

#define SPEED_FULL 1
#define SPEED_LOW 2
#define SPEED_HIGH 3
#define SPEED_SUPER 4

#define EP_TYPE_CONTROL 4
#define EP_TYPE_INTERRUPT_IN 7

struct erst_entry {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
};

struct xhci {
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *doorbells;
    uint32_t max_slots, max_ports, ctx_size;
    int ac64; /* Can DMA above 4 GiB. */
    uint8_t bus, dev, func;

    uint64_t *dcbaa;
    struct usb_ring cmd;
    struct xhci_trb *events;
    uint64_t events_phys;
    uint32_t event_dequeue, event_cycle;

    /* The one command in flight (commands only run during enumeration). */
    uint64_t cmd_trb;
    volatile int cmd_done;
    uint32_t cmd_cc, cmd_slot;

    struct usb_device *devices[256];     /* By slot ID. */
    struct usb_device *port_devices[256]; /* By 0-based port. */
    uint8_t port_changed[256];            /* Set by port status events, handled in usb_poll(). */
};

static struct xhci controllers[MAX_CONTROLLERS];
static int controller_count;

static const char *const SPEED_NAMES[] = {"?", "full", "low", "high", "super"};

/* --- Registers. 64-bit ones are written as two halves, low first. --- */

static uint32_t rd32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static void wr32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

static void wr64(volatile uint8_t *base, uint32_t off, uint64_t v) {
    wr32(base, off, (uint32_t)v);
    wr32(base, off + 4, (uint32_t)(v >> 32));
}

/* Waits up to `ms` for (register & mask) == want. */
static int wait_reg(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want,
                    uint32_t ms) {
    for (uint32_t waited = 0;; waited++) {
        if ((rd32(base, off) & mask) == want) {
            return 0;
        }
        if (waited >= ms) {
            return -1;
        }
        timer_sleep_ms(1);
    }
}

/* --- Memory the controller reads and writes: whole zeroed pages, below
 * 4 GiB if it can't address more. --- */

static void *dma_page(struct xhci *hc, uint64_t *phys_out) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0 || (!hc->ac64 && phys + PMM_PAGE_SIZE > 0x100000000ull)) {
        return NULL;
    }
    void *virt = (void *)(uintptr_t)(phys + hhdm_request.response->offset);
    memset(virt, 0, PMM_PAGE_SIZE);
    *phys_out = phys;
    return virt;
}

static int ring_init(struct xhci *hc, struct usb_ring *r) {
    r->trbs = dma_page(hc, &r->phys);
    if (r->trbs == NULL) {
        return -1;
    }
    r->enqueue = 0;
    r->cycle = 1;
    /* The last slot links back to the start and flips the cycle bit. */
    r->trbs[RING_TRBS - 1].param = r->phys;
    r->trbs[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    return 0;
}

/* Writes one TRB at the enqueue position and returns its physical
 * address. The control word, with the cycle bit that hands the TRB to
 * the controller, is written last. */
static uint64_t ring_push(struct usb_ring *r, uint64_t param, uint32_t status, uint32_t control) {
    struct xhci_trb *t = &r->trbs[r->enqueue];
    uint64_t addr = r->phys + (uint64_t)r->enqueue * sizeof(struct xhci_trb);
    t->param = param;
    t->status = status;
    asm volatile("" ::: "memory");
    t->control = control | r->cycle;

    if (++r->enqueue == RING_TRBS - 1) {
        struct xhci_trb *link = &r->trbs[RING_TRBS - 1];
        asm volatile("" ::: "memory");
        link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE | r->cycle;
        r->enqueue = 0;
        r->cycle ^= 1;
    }
    return addr;
}

static void ring_doorbell(struct xhci *hc, uint32_t slot, uint32_t target) {
    asm volatile("mfence" ::: "memory");
    hc->doorbells[slot] = target;
}

/* --- Events --- */

static void handle_event(struct xhci *hc, const struct xhci_trb *ev) {
    uint32_t type = TRB_GET_TYPE(ev->control);
    uint32_t cc = ev->status >> 24;

    if (type == TRB_EVENT_COMMAND) {
        if (ev->param == hc->cmd_trb) {
            hc->cmd_cc = cc;
            hc->cmd_slot = ev->control >> 24;
            hc->cmd_done = 1;
        }
        return;
    }
    if (type == TRB_EVENT_PORT_STATUS) {
        /* Not handled here: enumerating needs to wait for events itself. */
        uint32_t port = (uint32_t)(ev->param >> 24) & 0xFF;
        if (port >= 1 && port <= hc->max_ports) {
            hc->port_changed[port - 1] = 1;
        }
        return;
    }
    if (type != TRB_EVENT_TRANSFER) {
        return;
    }

    struct usb_device *dev = hc->devices[ev->control >> 24];
    uint32_t dci = (ev->control >> 16) & 0x1F;
    if (dev == NULL) {
        return;
    }
    if (dci == 1) {
        dev->ctrl_cc = cc;
        dev->ctrl_done = 1;
        return;
    }
    struct usb_ring *r = &dev->rings[dci];
    if (dev->on_transfer[dci] != NULL && ev->param >= r->phys &&
        ev->param < r->phys + PMM_PAGE_SIZE) {
        /* The event names the TRB; the TRB holds the buffer and length. */
        const struct xhci_trb *t = &r->trbs[(ev->param - r->phys) / sizeof(struct xhci_trb)];
        uint32_t asked = t->status & 0x1FFFF;
        uint32_t residual = ev->status & 0xFFFFFF;
        dev->on_transfer[dci](dev, t->param, residual <= asked ? asked - residual : 0, cc);
    }
}

static void process_events(struct xhci *hc) {
    int any = 0;
    for (;;) {
        struct xhci_trb *ev = &hc->events[hc->event_dequeue];
        if ((ev->control & TRB_CYCLE) != hc->event_cycle) {
            break;
        }
        struct xhci_trb copy = *ev;
        if (++hc->event_dequeue == RING_TRBS) {
            hc->event_dequeue = 0;
            hc->event_cycle ^= 1;
        }
        any = 1;
        handle_event(hc, &copy);
    }
    if (any) {
        wr64(hc->rt, IR0_ERDP,
             (hc->events_phys + (uint64_t)hc->event_dequeue * sizeof(struct xhci_trb)) | ERDP_EHB);
    }
}

/* Processes events until *flag is set: spins briefly first (most things
 * finish in microseconds), then checks once a tick. */
static int wait_flag(struct xhci *hc, volatile int *flag, uint32_t timeout_ms) {
    for (int i = 0; i < 20000; i++) {
        process_events(hc);
        if (*flag) {
            return 0;
        }
        asm volatile("pause");
    }
    for (uint32_t waited = 0; waited < timeout_ms; waited += 1000 / TIMER_HZ) {
        timer_sleep_ms(1000 / TIMER_HZ);
        process_events(hc);
        if (*flag) {
            return 0;
        }
    }
    return -1;
}

/* Runs one command; returns its completion code (0 on timeout). */
static uint32_t run_command(struct xhci *hc, uint64_t param, uint32_t control) {
    hc->cmd_done = 0;
    hc->cmd_trb = ring_push(&hc->cmd, param, 0, control);
    ring_doorbell(hc, 0, 0);
    if (wait_flag(hc, &hc->cmd_done, 1000) != 0) {
        kprintf("xhci: command %u timed out\n", TRB_GET_TYPE(control));
        return 0;
    }
    return hc->cmd_cc;
}

/* --- Contexts (6.2). Input contexts have the input control context
 * first, so everything sits one context further in than in the output
 * (device) context. --- */

static uint32_t *in_ctrl(struct usb_device *dev) {
    return (uint32_t *)dev->in_ctx;
}

static uint32_t *in_slot(struct usb_device *dev) {
    return (uint32_t *)((uint8_t *)dev->in_ctx + dev->hc->ctx_size);
}

static uint32_t *in_ep(struct usb_device *dev, uint32_t dci) {
    return (uint32_t *)((uint8_t *)dev->in_ctx + (dci + 1) * dev->hc->ctx_size);
}

static void set_ep_dequeue(uint32_t *ep, const struct usb_ring *r) {
    uint64_t dq = r->phys | 1; /* Dequeue cycle state starts at 1. */
    ep[2] = (uint32_t)dq;
    ep[3] = (uint32_t)(dq >> 32);
}

/* --- Control transfers --- */

/* After a STALL the endpoint is halted until reset, and its dequeue
 * pointer has to be moved past the failed transfer (4.6.8). */
static void recover_endpoint(struct usb_device *dev, uint32_t dci) {
    struct usb_ring *r = &dev->rings[dci];
    run_command(dev->hc, 0,
                TRB_TYPE(TRB_RESET_ENDPOINT) | (dci << 16) | ((uint32_t)dev->slot << 24));
    uint64_t dq = (r->phys + (uint64_t)r->enqueue * sizeof(struct xhci_trb)) | r->cycle;
    run_command(dev->hc, dq,
                TRB_TYPE(TRB_SET_TR_DEQUEUE) | (dci << 16) | ((uint32_t)dev->slot << 24));
}

int usb_control(struct usb_device *dev, uint8_t request_type, uint8_t request, uint16_t value,
                uint16_t index, void *data, uint16_t length) {
    if (length > PMM_PAGE_SIZE) {
        return -1;
    }
    int in = (request_type & 0x80) != 0;
    if (!in && length > 0) {
        memcpy(dev->buf, data, length);
    }

    struct usb_ring *r = &dev->rings[1];
    uint64_t setup = request_type | ((uint64_t)request << 8) | ((uint64_t)value << 16) |
                     ((uint64_t)index << 32) | ((uint64_t)length << 48);
    uint32_t trt = length == 0 ? 0 : (in ? 3 : 2); /* Transfer type: none / IN / OUT data. */
    ring_push(r, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (length > 0) {
        ring_push(r, dev->buf_phys, length, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0));
    }
    /* The status stage runs opposite to the data stage (IN if none). */
    uint32_t status_dir = (length == 0 || !in) ? TRB_DIR_IN : 0;
    ring_push(r, 0, 0, TRB_TYPE(TRB_STATUS) | status_dir | TRB_IOC);

    dev->ctrl_done = 0;
    ring_doorbell(dev->hc, dev->slot, 1);
    if (wait_flag(dev->hc, &dev->ctrl_done, 1000) != 0) {
        kprintf("usb: port %u: control request 0x%x timed out\n", dev->port, request);
        return -1;
    }
    if (dev->ctrl_cc != CC_SUCCESS && dev->ctrl_cc != CC_SHORT_PACKET) {
        if (dev->ctrl_cc == CC_STALL) {
            recover_endpoint(dev, 1);
        }
        return -1;
    }
    if (in && length > 0) {
        memcpy(data, dev->buf, length);
    }
    return 0;
}

/* --- Interrupt endpoints --- */

/* xHCI wants the polling interval as 2^n units of 125 us; full/low speed
 * descriptors give it in 1 ms frames, high/super speed already as an
 * exponent (plus one). */
static uint32_t xhci_interval(uint8_t speed, uint8_t interval) {
    if (speed == SPEED_HIGH || speed == SPEED_SUPER) {
        uint32_t n = interval > 0 ? interval - 1u : 0;
        return n > 15 ? 15 : n;
    }
    uint32_t frames_125us = (interval ? interval : 1) * 8u;
    uint32_t n = 0;
    while ((2u << n) <= frames_125us) {
        n++;
    }
    return n < 3 ? 3 : (n > 10 ? 10 : n);
}

int usb_add_interrupt_in(struct usb_device *dev, uint8_t ep_address, uint16_t max_packet,
                         uint8_t interval, usb_transfer_fn fn) {
    uint32_t dci = (ep_address & 0x0F) * 2u + 1;
    if (ring_init(dev->hc, &dev->rings[dci]) != 0) {
        return -1;
    }

    memset(dev->in_ctx, 0, PMM_PAGE_SIZE);
    in_ctrl(dev)[1] = 1u | (1u << dci); /* Add: slot context + this endpoint. */
    memcpy(in_slot(dev), dev->out_ctx, dev->hc->ctx_size);
    uint32_t entries = (in_slot(dev)[0] >> 27) & 0x1F;
    if (dci > entries) {
        in_slot(dev)[0] = (in_slot(dev)[0] & ~(0x1Fu << 27)) | (dci << 27);
    }

    uint32_t *ep = in_ep(dev, dci);
    ep[0] = xhci_interval(dev->speed, interval) << 16;
    ep[1] = (3u << 1) | (EP_TYPE_INTERRUPT_IN << 3) | ((uint32_t)max_packet << 16);
    set_ep_dequeue(ep, &dev->rings[dci]);
    ep[4] = max_packet | ((uint32_t)max_packet << 16); /* Average TRB length, max ESIT payload. */

    uint32_t cc = run_command(dev->hc, dev->in_ctx_phys,
                              TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)dev->slot << 24));
    if (cc != CC_SUCCESS) {
        kprintf("usb: port %u: configuring endpoint 0x%x failed (%u)\n", dev->port, ep_address,
                cc);
        return -1;
    }
    dev->ep_mps[dci] = max_packet;
    dev->on_transfer[dci] = fn;
    return (int)dci;
}

void usb_queue_in(struct usb_device *dev, int dci, uint64_t buf_phys, uint16_t length) {
    ring_push(&dev->rings[dci], buf_phys, length, TRB_TYPE(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    ring_doorbell(dev->hc, dev->slot, (uint32_t)dci);
}

/* --- Enumeration --- */

/* Resets a USB 2 port, which is what enables it; a USB 3 port enables
 * itself during link training, so it's already enabled by now. */
static int port_reset(struct xhci *hc, uint32_t i) {
    uint32_t sc = rd32(hc->op, OP_PORTSC(i));
    if (sc & PORTSC_PED) {
        return 0;
    }
    wr32(hc->op, OP_PORTSC(i), PORTSC_KEEP(sc) | PORTSC_PR);
    if (wait_reg(hc->op, OP_PORTSC(i), PORTSC_PRC, PORTSC_PRC, 500) != 0) {
        return -1;
    }
    sc = rd32(hc->op, OP_PORTSC(i));
    wr32(hc->op, OP_PORTSC(i), PORTSC_KEEP(sc) | PORTSC_PRC);
    timer_sleep_ms(20); /* Reset recovery (USB 2.0 7.1.7.5: 10 ms minimum). */
    return (rd32(hc->op, OP_PORTSC(i)) & PORTSC_PED) ? 0 : -1;
}

static struct usb_device *new_device(struct xhci *hc, uint32_t port_index, uint8_t speed) {
    struct usb_device *dev = kmalloc(sizeof(*dev));
    if (dev == NULL) {
        return NULL;
    }
    memset(dev, 0, sizeof(*dev));
    dev->hc = hc;
    dev->port = (uint8_t)(port_index + 1);
    dev->speed = speed;
    dev->in_ctx = dma_page(hc, &dev->in_ctx_phys);
    dev->out_ctx = dma_page(hc, &dev->out_ctx_phys);
    dev->buf = dma_page(hc, &dev->buf_phys);
    if (dev->in_ctx == NULL || dev->out_ctx == NULL || dev->buf == NULL ||
        ring_init(hc, &dev->rings[1]) != 0) {
        return NULL;
    }
    switch (speed) {
        case SPEED_HIGH:
            dev->mps0 = 64;
            break;
        case SPEED_SUPER:
            dev->mps0 = 512;
            break;
        default:
            dev->mps0 = 8; /* Low speed always; full speed until the descriptor says. */
            break;
    }
    return dev;
}

static int address_device(struct usb_device *dev) {
    struct xhci *hc = dev->hc;
    uint32_t cc = run_command(hc, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    if (cc != CC_SUCCESS) {
        kprintf("usb: port %u: no free device slot (%u)\n", dev->port, cc);
        return -1;
    }
    dev->slot = (uint8_t)hc->cmd_slot;
    hc->devices[dev->slot] = dev;
    hc->dcbaa[dev->slot] = dev->out_ctx_phys;

    in_ctrl(dev)[1] = 0x3; /* Add: slot context + endpoint 0. */
    in_slot(dev)[0] = ((uint32_t)dev->speed << 20) | (1u << 27); /* One context entry. */
    in_slot(dev)[1] = (uint32_t)dev->port << 16;                  /* Root hub port. */
    uint32_t *ep0 = in_ep(dev, 1);
    ep0[1] = (3u << 1) | (EP_TYPE_CONTROL << 3) | ((uint32_t)dev->mps0 << 16);
    set_ep_dequeue(ep0, &dev->rings[1]);
    ep0[4] = 8; /* Average TRB length. */

    cc = run_command(hc, dev->in_ctx_phys,
                     TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)dev->slot << 24));
    if (cc != CC_SUCCESS) {
        kprintf("usb: port %u: Address Device failed (%u)\n", dev->port, cc);
        return -1;
    }
    timer_sleep_ms(2); /* SET_ADDRESS recovery (USB 2.0 9.2.6.3). */
    return 0;
}

/* Full-speed devices may use an endpoint 0 packet size other than the 8
 * bytes assumed until the first 8 bytes of the descriptor are in. */
static int fix_mps0(struct usb_device *dev, uint16_t mps0) {
    if (mps0 == dev->mps0 || mps0 == 0) {
        return 0;
    }
    dev->mps0 = mps0;
    memset(dev->in_ctx, 0, PMM_PAGE_SIZE);
    in_ctrl(dev)[1] = 1u << 1; /* Evaluate endpoint 0 only. */
    uint32_t *ep0 = in_ep(dev, 1);
    memcpy(ep0, (uint8_t *)dev->out_ctx + dev->hc->ctx_size, dev->hc->ctx_size);
    ep0[1] = (ep0[1] & 0x0000FFFFu) | ((uint32_t)mps0 << 16);
    uint32_t cc = run_command(dev->hc, dev->in_ctx_phys,
                              TRB_TYPE(TRB_EVALUATE_CONTEXT) | ((uint32_t)dev->slot << 24));
    return cc == CC_SUCCESS ? 0 : -1;
}

static void enumerate_port(struct xhci *hc, uint32_t i) {
    if (port_reset(hc, i) != 0) {
        kprintf("usb: port %u: reset failed\n", i + 1);
        return;
    }
    uint8_t speed = (rd32(hc->op, OP_PORTSC(i)) >> 10) & 0xF;
    struct usb_device *dev = new_device(hc, i, speed);
    if (dev == NULL) {
        kprintf("usb: port %u: out of memory\n", i + 1);
        return;
    }
    hc->port_devices[i] = dev;
    if (address_device(dev) != 0) {
        return;
    }

    uint8_t desc[18];
    if (usb_control(dev, 0x80, 6, 0x0100, 0, desc, 8) != 0) { /* GET_DESCRIPTOR(device), 8 */
        kprintf("usb: port %u: no device descriptor\n", dev->port);
        return;
    }
    uint16_t mps0 = speed == SPEED_SUPER ? (uint16_t)(1u << desc[7]) : desc[7];
    if (fix_mps0(dev, mps0) != 0 || usb_control(dev, 0x80, 6, 0x0100, 0, desc, 18) != 0) {
        kprintf("usb: port %u: device descriptor failed\n", dev->port);
        return;
    }
    dev->dev_class = desc[4];
    dev->vendor = (uint16_t)(desc[8] | (desc[9] << 8));
    dev->product = (uint16_t)(desc[10] | (desc[11] << 8));
    kprintf("usb: port %u: %x:%x, %s speed, class %u\n", dev->port, dev->vendor,
            dev->product, speed <= SPEED_SUPER ? SPEED_NAMES[speed] : "?", dev->dev_class);
    if (dev->dev_class == 9) {
        kprintf("usb: port %u: a USB hub -- not supported yet, so nothing behind it works; "
                "plug the keyboard straight into the PC\n",
                dev->port);
        return;
    }

    uint8_t *config = kmalloc(PMM_PAGE_SIZE);
    if (config == NULL || usb_control(dev, 0x80, 6, 0x0200, 0, config, 9) != 0) {
        kprintf("usb: port %u: no configuration descriptor\n", dev->port);
        kfree(config);
        return;
    }
    uint16_t total = (uint16_t)(config[2] | (config[3] << 8));
    if (total > PMM_PAGE_SIZE) {
        total = PMM_PAGE_SIZE;
    }
    if (usb_control(dev, 0x80, 6, 0x0200, 0, config, total) == 0) {
        usb_kbd_probe(dev, config, total);
    }
    kfree(config);
}

/* Takes the controller from the firmware's "USB legacy support" -- the
 * SMM code that emulates a PS/2 keyboard for the boot menu -- which
 * otherwise keeps fighting the driver for it (4.22.1). */
static void bios_handoff(struct xhci *hc) {
    uint32_t off = ((rd32(hc->cap, CAP_HCCPARAMS1) >> 16) & 0xFFFF) * 4;
    while (off != 0) {
        uint32_t v = rd32(hc->cap, off);
        if ((v & 0xFF) == 1) {
            if (v & (1u << 16)) {
                hc->cap[off + 3] = 1; /* OS-owned semaphore. */
                if (wait_reg(hc->cap, off, 1u << 16, 0, 1000) != 0) {
                    kprintf("xhci: firmware won't release the controller -- taking it anyway\n");
                    hc->cap[off + 2] = 0;
                }
            }
            /* Disable the firmware's SMIs, clearing any it left pending. */
            wr32(hc->cap, off + 4, (rd32(hc->cap, off + 4) & 0xFFFF1FEEu) | 0xE0000000u);
            return;
        }
        uint32_t next = (v >> 8) & 0xFF;
        off = next ? off + next * 4 : 0;
    }
}

static int start_controller(struct xhci *hc, const struct pci_device *pci) {
    hc->bus = pci->bus;
    hc->dev = pci->slot;
    hc->func = pci->func;

    /* Memory decoding + bus mastering on, legacy INTx off (we poll). */
    uint16_t cmd = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
    pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, (uint16_t)(cmd | 0x0406));

    uint64_t bar = pci->bar[0] & ~0xFull;
    if ((pci->bar[0] & 0x6) == 0x4) {
        bar |= (uint64_t)pci->bar[1] << 32;
    }
    volatile uint8_t *probe = (volatile uint8_t *)vmm_map_mmio(bar, 0x1000);
    uint32_t caplen = probe[CAP_CAPLENGTH];
    uint32_t hcs1 = rd32(probe, CAP_HCSPARAMS1);
    uint32_t dboff = rd32(probe, CAP_DBOFF) & ~0x3u;
    uint32_t rtsoff = rd32(probe, CAP_RTSOFF) & ~0x1Fu;
    hc->max_slots = hcs1 & 0xFF;
    hc->max_ports = (hcs1 >> 24) & 0xFF;

    uint64_t size = caplen + OP_PORTSC(hc->max_ports);
    if (rtsoff + 0x40 > size) {
        size = rtsoff + 0x40;
    }
    if (dboff + (hc->max_slots + 1) * 4 > size) {
        size = dboff + (hc->max_slots + 1) * 4;
    }
    hc->cap = (volatile uint8_t *)vmm_map_mmio(bar, size);
    hc->op = hc->cap + caplen;
    hc->rt = hc->cap + rtsoff;
    hc->doorbells = (volatile uint32_t *)(hc->cap + dboff);

    uint32_t hcc1 = rd32(hc->cap, CAP_HCCPARAMS1);
    hc->ac64 = hcc1 & 1;
    hc->ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;

    bios_handoff(hc);

    /* Stop, then reset. */
    wr32(hc->op, OP_USBCMD, rd32(hc->op, OP_USBCMD) & ~USBCMD_RUN);
    if (wait_reg(hc->op, OP_USBSTS, USBSTS_HCH, USBSTS_HCH, 100) != 0) {
        kprintf("xhci: controller won't halt\n");
        return -1;
    }
    wr32(hc->op, OP_USBCMD, USBCMD_HCRST);
    timer_sleep_ms(1);
    if (wait_reg(hc->op, OP_USBCMD, USBCMD_HCRST, 0, 1000) != 0 ||
        wait_reg(hc->op, OP_USBSTS, USBSTS_CNR, 0, 1000) != 0) {
        kprintf("xhci: controller reset timed out\n");
        return -1;
    }

    wr32(hc->op, OP_CONFIG, hc->max_slots);

    uint64_t phys;
    hc->dcbaa = dma_page(hc, &phys);
    if (hc->dcbaa == NULL) {
        return -1;
    }
    /* Scratchpad: pages the controller keeps for itself (4.20). */
    uint32_t hcs2 = rd32(hc->cap, CAP_HCSPARAMS2);
    uint32_t scratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (scratch > 0) {
        uint64_t array_phys;
        uint64_t *array = dma_page(hc, &array_phys);
        if (array == NULL) {
            return -1;
        }
        for (uint32_t i = 0; i < scratch; i++) {
            if (dma_page(hc, &array[i]) == NULL) {
                return -1;
            }
        }
        hc->dcbaa[0] = array_phys;
    }
    wr64(hc->op, OP_DCBAAP, phys);

    if (ring_init(hc, &hc->cmd) != 0) {
        return -1;
    }
    wr64(hc->op, OP_CRCR, hc->cmd.phys | 1);

    hc->events = dma_page(hc, &hc->events_phys);
    uint64_t erst_phys;
    struct erst_entry *erst = dma_page(hc, &erst_phys);
    if (hc->events == NULL || erst == NULL) {
        return -1;
    }
    erst[0].base = hc->events_phys;
    erst[0].size = RING_TRBS;
    hc->event_dequeue = 0;
    hc->event_cycle = 1;
    wr32(hc->rt, IR0_ERSTSZ, 1);
    wr64(hc->rt, IR0_ERDP, hc->events_phys);
    wr64(hc->rt, IR0_ERSTBA, erst_phys);
    wr32(hc->rt, IR0_IMAN, 1); /* Clear any pending flag; interrupts stay off. */

    wr32(hc->op, OP_USBCMD, USBCMD_RUN);
    if (wait_reg(hc->op, OP_USBSTS, USBSTS_HCH, 0, 100) != 0) {
        kprintf("xhci: controller won't start\n");
        return -1;
    }

    /* Power the ports if that's software's job (HCCPARAMS1.PPC), then
     * give devices time to connect and USB 3 links time to train. */
    if (hcc1 & (1u << 3)) {
        for (uint32_t i = 0; i < hc->max_ports; i++) {
            uint32_t sc = rd32(hc->op, OP_PORTSC(i));
            if (!(sc & PORTSC_PP)) {
                wr32(hc->op, OP_PORTSC(i), PORTSC_KEEP(sc) | PORTSC_PP);
            }
        }
    }
    timer_sleep_ms(300);
    return 0;
}

/* A device was unplugged: stop routing its events and free its slot. Its
 * memory is leaked -- small, and only on unplug. */
static void release_port(struct xhci *hc, uint32_t i) {
    struct usb_device *dev = hc->port_devices[i];
    hc->port_devices[i] = NULL;
    dev->gone = 1;
    if (dev->slot != 0) {
        hc->devices[dev->slot] = NULL;
        run_command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)dev->slot << 24));
        hc->dcbaa[dev->slot] = 0;
    }
    kprintf("usb: port %u: unplugged\n", i + 1);
}

/* Clears the port's change bits -- a new status change event only comes
 * once they're clear -- then enumerates or releases it to match. */
static void port_changed(struct xhci *hc, uint32_t i) {
    uint32_t sc = rd32(hc->op, OP_PORTSC(i));
    wr32(hc->op, OP_PORTSC(i), PORTSC_KEEP(sc) | (sc & PORTSC_CHANGE_BITS));
    int connected = (sc & PORTSC_CCS) != 0;
    if (!connected && hc->port_devices[i] != NULL) {
        release_port(hc, i);
    } else if (connected && hc->port_devices[i] == NULL) {
        timer_sleep_ms(100); /* Connect debounce (USB 2.0 7.1.7.3). */
        enumerate_port(hc, i);
    }
}

int usb_init(void) {
    for (int n = 0;; n++) {
        const struct pci_device *pci = pci_find_class_nth(0x0C, 0x03, 0x30, n);
        if (pci == NULL) {
            break;
        }
        if (controller_count == MAX_CONTROLLERS) {
            kprintf("xhci: more than %d controllers -- ignoring the rest\n", MAX_CONTROLLERS);
            break;
        }
        struct xhci *hc = &controllers[controller_count];
        kprintf("xhci: controller at %u:%u.%u (%x:%x)\n", pci->bus, pci->slot, pci->func,
                pci->vendor_id, pci->device_id);
        if (start_controller(hc, pci) != 0) {
            memset(hc, 0, sizeof(*hc));
            continue;
        }
        controller_count++;

        uint32_t connected = 0;
        for (uint32_t i = 0; i < hc->max_ports; i++) {
            uint32_t sc = rd32(hc->op, OP_PORTSC(i));
            wr32(hc->op, OP_PORTSC(i), PORTSC_KEEP(sc) | (sc & PORTSC_CHANGE_BITS));
            if (sc & PORTSC_CCS) {
                connected++;
                enumerate_port(hc, i);
            }
        }
        process_events(hc);
        memset(hc->port_changed, 0, sizeof(hc->port_changed)); /* Just handled them all. */
        kprintf("xhci: %u:%u.%u: %u ports, %u connected\n", hc->bus, hc->dev, hc->func,
                hc->max_ports, connected);
    }
    return controller_count;
}

void usb_poll(void) {
    for (int c = 0; c < controller_count; c++) {
        struct xhci *hc = &controllers[c];
        process_events(hc);
        for (uint32_t i = 0; i < hc->max_ports; i++) {
            if (hc->port_changed[i]) {
                hc->port_changed[i] = 0;
                port_changed(hc, i);
            }
        }
    }
}
