#ifndef DRIVERS_USB_USB_H
#define DRIVERS_USB_USB_H

#include <stdint.h>

/* USB on xHCI, the controller every PC from the last decade has (QEMU:
 * -device qemu-xhci). Polled, like every other driver here: nothing uses
 * interrupts, usb_poll() drains each controller's event ring.
 *
 * Only devices plugged straight into a controller's own ports are found
 * -- no hub support yet, so a device behind a hub (including many front
 * panels and monitors) is logged and skipped. */

#define USB_MAX_ENDPOINTS 32 /* xHCI device context index (DCI) range. */

struct xhci;
struct usb_device;

/* Called when a transfer queued with usb_queue_in() completes: `buf_phys`
 * is the buffer it was queued with, `len` how many bytes arrived, `cc`
 * the xHCI completion code (1 = success, 13 = short packet, both fine). */
typedef void (*usb_transfer_fn)(struct usb_device *dev, uint64_t buf_phys, uint32_t len,
                                uint32_t cc);

struct usb_device {
    struct xhci *hc;
    uint8_t slot;
    uint8_t port;  /* 1-based root hub port. */
    uint8_t speed; /* xHCI speed ID: 1 full, 2 low, 3 high, 4 super. */
    uint16_t vendor, product;
    uint8_t dev_class;
    volatile int gone; /* Unplugged: queue nothing more. */

    /* Everything below is the xHCI driver's own. */
    uint16_t mps0;
    void *in_ctx, *out_ctx, *buf; /* One page each; `buf` bounces control transfers. */
    uint64_t in_ctx_phys, out_ctx_phys, buf_phys;
    struct usb_ring {
        struct xhci_trb *trbs;
        uint64_t phys;
        uint32_t enqueue;
        uint32_t cycle;
    } rings[USB_MAX_ENDPOINTS];
    uint16_t ep_mps[USB_MAX_ENDPOINTS];
    usb_transfer_fn on_transfer[USB_MAX_ENDPOINTS];
    volatile int ctrl_done;
    uint32_t ctrl_cc;
    void *driver_data;
};

/* Finds every xHCI controller (PCI class 0C.03, prog-if 30), takes it
 * over from the firmware, resets it, and enumerates whatever is plugged
 * into its ports, offering each device to the class drivers (just the
 * keyboard, drivers/usb/usb_kbd.c, so far). pci_enumerate() and the timer
 * must be up. Returns the number of controllers started. */
int usb_init(void);

/* Processes any pending controller events -- completed transfers land in
 * their usb_transfer_fn here. Cheap when nothing happened. */
void usb_poll(void);

/* A control transfer on endpoint 0. `data` is ordinary kernel memory,
 * up to a page. Returns 0 on success, -1 on any failure (including a
 * STALL, after which the endpoint is reset and usable again). */
int usb_control(struct usb_device *dev, uint8_t request_type, uint8_t request, uint16_t value,
                uint16_t index, void *data, uint16_t length);

/* Configures interrupt IN endpoint `ep_address` (from its endpoint
 * descriptor) and sets `fn` to receive its completions. Returns the
 * endpoint's DCI for usb_queue_in(), or -1. */
int usb_add_interrupt_in(struct usb_device *dev, uint8_t ep_address, uint16_t max_packet,
                         uint8_t interval, usb_transfer_fn fn);

/* Queues one IN transfer into `buf_phys` (DMA-able memory, e.g. from the
 * PMM). It completes through the endpoint's usb_transfer_fn. */
void usb_queue_in(struct usb_device *dev, int dci, uint64_t buf_phys, uint16_t length);

#endif
