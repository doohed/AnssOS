#include "hda.h"
#include "pci.h"
#include "serial.h"
#include "timer.h"
#include "../boot/requests.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

#include <stddef.h>
#include <stdint.h>

/* Register and verb names follow the HD Audio spec (Intel, rev 1.0a);
 * section numbers are given where it helps. */

/* Controller registers (3.3). */
#define GCAP 0x00
#define GCTL 0x08
#define STATESTS 0x0E
#define INTCTL 0x20
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP 0x48
#define CORBRP 0x4A
#define CORBCTL 0x4C
#define CORBSIZE 0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP 0x58
#define RINTCNT 0x5A
#define RIRBCTL 0x5C
#define RIRBSTS 0x5D
#define RIRBSIZE 0x5E

/* Stream descriptor registers, relative to the descriptor (3.3.35). */
#define SD_BASE(n) (0x80 + 0x20 * (n))
#define SD_CTL 0x00 /* 3 bytes: SRST, RUN, ..., stream tag in bits 23:20. */
#define SD_STS 0x03
#define SD_LPIB 0x04
#define SD_CBL 0x08
#define SD_LVI 0x0C
#define SD_FMT 0x12
#define SD_BDPL 0x18
#define SD_BDPU 0x1C
#define SD_CTL_SRST 0x01
#define SD_CTL_RUN 0x02

/* Verbs (7.3). 12-bit verbs carry an 8-bit payload, 4-bit ones 16 bits. */
#define VERB_GET_PARAM 0xF00
#define VERB_GET_CONN_LIST 0xF02
#define VERB_SET_CONN_SELECT 0x701
#define VERB_SET_POWER_STATE 0x705
#define VERB_SET_STREAM_CHANNEL 0x706
#define VERB_SET_PIN_CONTROL 0x707
#define VERB_SET_EAPD 0x70C
#define VERB_GET_CONFIG_DEFAULT 0xF1C
#define VERB4_SET_FORMAT 0x2
#define VERB4_SET_AMP 0x3

/* Parameters (7.3.4). */
#define PARAM_VENDOR_ID 0x00
#define PARAM_NODE_COUNT 0x04
#define PARAM_FUNCTION_TYPE 0x05
#define PARAM_WIDGET_CAPS 0x09
#define PARAM_PCM_RATES 0x0A
#define PARAM_PIN_CAPS 0x0C
#define PARAM_IN_AMP_CAPS 0x0D
#define PARAM_CONN_LIST_LEN 0x0E
#define PARAM_OUT_AMP_CAPS 0x12

/* Widget types (7.3.4.6). */
#define WIDGET_OUTPUT 0
#define WIDGET_MIXER 2
#define WIDGET_SELECTOR 3
#define WIDGET_PIN 4

#define WCAP_IN_AMP (1u << 1)
#define WCAP_OUT_AMP (1u << 2)
#define WCAP_AMP_OVERRIDE (1u << 3)
#define WCAP_DIGITAL (1u << 9)
#define WCAP_POWER (1u << 10)

#define PINCAP_OUT (1u << 4)
#define PINCAP_HDMI (1u << 7)
#define PINCAP_EAPD (1u << 16)
#define PINCAP_DP (1u << 24)

#define DEVICE_LINE_OUT 0
#define DEVICE_SPEAKER 1
#define DEVICE_HEADPHONE 2

#define MAX_NODES 256
#define MAX_CONNS 16
#define MAX_DACS 8
#define PATH_MAX_DEPTH 6

#define STREAM_TAG 1
#define BUFFER_BYTES 32768 /* ~170 ms of 48 kHz stereo -- same as virtio-sound's. */
#define BDL_ENTRIES 8
#define GUARD_BYTES 1024 /* Never write this close behind the DMA: it reads ahead. */

struct node {
    uint32_t caps;
    uint32_t pin_caps;
    uint32_t config;
    uint8_t conn_count;
    uint8_t conns[MAX_CONNS];
};

struct bdl_entry {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
};

/* The one controller and codec in use. */
static volatile uint8_t *regs;
static uint32_t codec;
static uint32_t afg;
static uint32_t out_amp_default, in_amp_default; /* The AFG's, for widgets without their own. */
static struct node nodes[MAX_NODES];
static uint8_t dacs[MAX_DACS];
static int dac_count;

static uint32_t *corb;
static uint64_t *rirb;
static uint32_t ring_entries;
static uint32_t rirb_read;
static int command_timeouts; /* A codec that stops answering is given up on. */

static uint32_t stream_base; /* SD_BASE() of the first output stream. */
static uint8_t *buffer;      /* BUFFER_BYTES, contiguous. */
static uint64_t buffer_phys;
static struct bdl_entry *bdl;
static uint64_t bdl_phys;

static int ready, open, running, mono;
static uint64_t written, played; /* Bytes, since hda_open(). */
static uint32_t last_lpib;

/* --- Registers --- */

static uint8_t rd8(uint32_t off) {
    return regs[off];
}
static uint16_t rd16(uint32_t off) {
    return *(volatile uint16_t *)(regs + off);
}
static uint32_t rd32(uint32_t off) {
    return *(volatile uint32_t *)(regs + off);
}
static void wr8(uint32_t off, uint8_t v) {
    regs[off] = v;
}
static void wr16(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(regs + off) = v;
}
static void wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(regs + off) = v;
}

static int wait8(uint32_t off, uint8_t mask, uint8_t want, uint32_t ms) {
    for (uint32_t waited = 0;; waited++) {
        if ((rd8(off) & mask) == want) {
            return 0;
        }
        if (waited >= ms) {
            return -1;
        }
        timer_sleep_ms(1);
    }
}

/* The controller may not snoop CPU caches (AMD's, unless told to), so
 * anything it reads gets written back to RAM first, and anything it
 * wrote is evicted before the CPU reads it. */
static void flush(const void *p, uint64_t len) {
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63;
    for (; a < (uintptr_t)p + len; a += 64) {
        asm volatile("clflush (%0)" : : "r"(a) : "memory");
    }
    asm volatile("mfence" ::: "memory");
}

static void *dma_pages(uint64_t count, uint64_t *phys_out) {
    uint64_t phys = count == 1 ? pmm_alloc_page() : pmm_alloc_pages(count);
    if (phys == 0) {
        return NULL;
    }
    void *virt = (void *)(uintptr_t)(phys + hhdm_request.response->offset);
    memset(virt, 0, count * PMM_PAGE_SIZE);
    flush(virt, count * PMM_PAGE_SIZE);
    *phys_out = phys;
    return virt;
}

/* --- Codec commands, through the CORB/RIRB rings (4.4.1) --- */

/* Sends one verb and waits for its response. The controller stops
 * delivering responses once RINTCNT of them are unacknowledged (that's
 * what drives its interrupt), so each one is acknowledged in RIRBSTS --
 * the interrupt handler's job in a driver that has one. */
static uint32_t command(uint32_t nid, uint32_t verb_and_payload) {
    if (command_timeouts > 0) {
        return 0xFFFFFFFFu;
    }
    uint32_t wp = (rd16(CORBWP) + 1) % ring_entries;
    corb[wp] = (codec << 28) | (nid << 20) | verb_and_payload;
    flush(&corb[wp], 4);
    wr16(CORBWP, (uint16_t)wp);

    for (uint32_t tries = 0; tries < 20000 + 50; tries++) {
        if ((rd16(RIRBWP) & 0xFF) % ring_entries != rirb_read) {
            rirb_read = (rirb_read + 1) % ring_entries;
            flush(&rirb[rirb_read], 8);
            uint64_t entry = rirb[rirb_read];
            wr8(RIRBSTS, 0x05); /* Response interrupt + overrun flags. */
            if ((entry >> 36) & 1) {
                continue; /* Unsolicited (a jack event) -- not our answer. */
            }
            return (uint32_t)entry;
        }
        if (tries >= 20000) {
            timer_sleep_ms(1); /* Fast polls first; then up to ~50 ticks. */
        }
    }
    command_timeouts++;
    kprintf("hda: codec %u stopped answering (verb 0x%x to node 0x%x)\n", codec,
            verb_and_payload, nid);
    return 0xFFFFFFFFu;
}

static uint32_t verb(uint32_t nid, uint32_t v, uint32_t payload8) {
    return command(nid, (v << 8) | (payload8 & 0xFF));
}

static uint32_t verb4(uint32_t nid, uint32_t v, uint32_t payload16) {
    return command(nid, (v << 16) | (payload16 & 0xFFFF));
}

static uint32_t param(uint32_t nid, uint32_t p) {
    return verb(nid, VERB_GET_PARAM, p);
}

/* --- Controller --- */

static int reset_controller(void) {
    wr32(GCTL, rd32(GCTL) & ~1u);
    if (wait8(GCTL, 1, 0, 100) != 0) {
        return -1;
    }
    timer_sleep_ms(1);
    wr32(GCTL, rd32(GCTL) | 1u);
    if (wait8(GCTL, 1, 1, 100) != 0) {
        return -1;
    }
    timer_sleep_ms(10); /* Codecs announce themselves within 521 us of reset (4.3). */
    return 0;
}

static int start_rings(void) {
    uint64_t phys;
    uint8_t *page = dma_pages(1, &phys);
    if (page == NULL) {
        return -1;
    }
    corb = (uint32_t *)page;
    rirb = (uint64_t *)(page + 2048);

    uint8_t caps = rd8(CORBSIZE) >> 4;
    uint8_t size_sel = (caps & 4) ? 2 : (caps & 2) ? 1 : 0;
    ring_entries = size_sel == 2 ? 256 : size_sel == 1 ? 16 : 2;

    wr8(CORBCTL, 0);
    wr8(RIRBCTL, 0);
    wr8(CORBSIZE, (uint8_t)((rd8(CORBSIZE) & ~3u) | size_sel));
    wr8(RIRBSIZE, (uint8_t)((rd8(RIRBSIZE) & ~3u) | size_sel));
    wr32(CORBLBASE, (uint32_t)phys);
    wr32(CORBUBASE, (uint32_t)(phys >> 32));
    wr32(RIRBLBASE, (uint32_t)(phys + 2048));
    wr32(RIRBUBASE, (uint32_t)((phys + 2048) >> 32));

    /* Read-pointer reset handshake. Some controllers (AMD's among them)
     * clear the bit by themselves, so neither wait is fatal. */
    wr16(CORBRP, 0x8000);
    for (int i = 0; i < 50 && !(rd16(CORBRP) & 0x8000); i++) {
        timer_sleep_ms(1);
    }
    wr16(CORBRP, 0);
    for (int i = 0; i < 50 && (rd16(CORBRP) & 0x8000); i++) {
        timer_sleep_ms(1);
    }
    wr16(CORBWP, 0);
    wr16(RIRBWP, 0x8000);
    wr16(RINTCNT, 1);
    wr8(RIRBSTS, 0x05);
    rirb_read = 0;

    /* DMA run. The RIRB also gets its response-interrupt enable, as in
     * Linux: without it the response flag command() acknowledges is never
     * raised, and some controllers (QEMU's) stop answering after RINTCNT
     * responses. No interrupt reaches the CPU: INTCTL's global enable
     * stays off and PCI INTx is disabled. */
    wr8(CORBCTL, 0x02);
    wr8(RIRBCTL, 0x03);
    return 0;
}

/* --- Codec graph --- */

static void read_connections(uint32_t nid) {
    struct node *n = &nodes[nid];
    uint32_t len_param = param(nid, PARAM_CONN_LIST_LEN);
    uint32_t len = len_param & 0x7F;
    int long_form = (len_param >> 7) & 1;
    uint32_t per = long_form ? 2 : 4;
    uint32_t bits = long_form ? 16 : 8;
    uint32_t prev = 0;
    n->conn_count = 0;

    for (uint32_t i = 0; i < len; i += per) {
        uint32_t resp = verb(nid, VERB_GET_CONN_LIST, i);
        for (uint32_t j = 0; j < per && i + j < len; j++) {
            uint32_t e = (resp >> (j * bits)) & ((1u << bits) - 1);
            uint32_t range = e & (1u << (bits - 1));
            e &= (1u << (bits - 1)) - 1;
            /* A range entry means "everything from the previous entry up
             * to this one" (7.3.3.3). */
            uint32_t from = range && prev ? prev + 1 : e;
            for (uint32_t v = from; v <= e && n->conn_count < MAX_CONNS; v++) {
                n->conns[n->conn_count++] = (uint8_t)v;
            }
            prev = e;
        }
    }
}

static uint32_t type_of(uint32_t nid) {
    return (nodes[nid].caps >> 20) & 0xF;
}

/* Depth-first from `nid` towards a DAC; fills path[0..depth] with the
 * nodes and choice[] with which connection each one takes. Returns the
 * path length, or 0 if no DAC is reachable. */
static int find_dac(uint32_t nid, int depth, uint8_t *path, uint8_t *choice) {
    path[depth] = (uint8_t)nid;
    uint32_t t = type_of(nid);
    if (t == WIDGET_OUTPUT) {
        return (nodes[nid].caps & WCAP_DIGITAL) ? 0 : depth + 1;
    }
    if (depth + 1 >= PATH_MAX_DEPTH || (depth > 0 && t != WIDGET_MIXER && t != WIDGET_SELECTOR)) {
        return 0;
    }
    for (uint32_t i = 0; i < nodes[nid].conn_count; i++) {
        choice[depth] = (uint8_t)i;
        int len = find_dac(nodes[nid].conns[i], depth + 1, path, choice);
        if (len > 0) {
            return len;
        }
    }
    return 0;
}

/* 0 dB: the amp capability's offset field (7.3.4.10). */
static uint32_t zero_db(uint32_t nid, int input) {
    uint32_t caps = nodes[nid].caps & WCAP_AMP_OVERRIDE
                        ? param(nid, input ? PARAM_IN_AMP_CAPS : PARAM_OUT_AMP_CAPS)
                        : (input ? in_amp_default : out_amp_default);
    uint32_t offset = caps & 0x7F;
    uint32_t steps = (caps >> 8) & 0x7F;
    return offset > steps ? steps : offset;
}

static void unmute_out(uint32_t nid) {
    if (nodes[nid].caps & WCAP_OUT_AMP) {
        verb4(nid, VERB4_SET_AMP, 0xB000 | zero_db(nid, 0)); /* Output, left + right. */
    }
}

static void unmute_in(uint32_t nid, uint32_t index) {
    if (nodes[nid].caps & WCAP_IN_AMP) {
        verb4(nid, VERB4_SET_AMP, 0x7000 | (index << 8) | zero_db(nid, 1));
    }
}

static void power_up(uint32_t nid) {
    if (nodes[nid].caps & WCAP_POWER) {
        verb(nid, VERB_SET_POWER_STATE, 0); /* D0 */
    }
}

static const char *device_name(uint32_t device) {
    switch (device) {
        case DEVICE_LINE_OUT:
            return "line out";
        case DEVICE_SPEAKER:
            return "speaker";
        default:
            return "headphones";
    }
}

/* Routes one output pin to a DAC: selects each hop, unmutes every amp
 * along the way, enables the pin (and its external amplifier). */
static int route_pin(uint32_t pin) {
    uint8_t path[PATH_MAX_DEPTH], choice[PATH_MAX_DEPTH];
    int len = find_dac(pin, 0, path, choice);
    if (len == 0) {
        return -1;
    }
    for (int i = 0; i < len; i++) {
        uint32_t nid = path[i];
        power_up(nid);
        if (i + 1 < len) {
            uint32_t t = type_of(nid);
            if (t == WIDGET_MIXER) {
                unmute_in(nid, choice[i]);
            } else if (nodes[nid].conn_count > 1) {
                verb(nid, VERB_SET_CONN_SELECT, choice[i]);
            }
        }
        unmute_out(nid);
    }

    uint32_t device = (nodes[pin].config >> 20) & 0xF;
    verb(pin, VERB_SET_PIN_CONTROL, device == DEVICE_HEADPHONE ? 0xC0 : 0x40); /* Out (+HP). */
    if (nodes[pin].pin_caps & PINCAP_EAPD) {
        verb(pin, VERB_SET_EAPD, 0x02);
    }

    uint32_t dac = path[len - 1];
    int known = 0;
    for (int i = 0; i < dac_count; i++) {
        known |= dacs[i] == dac;
    }
    if (!known && dac_count < MAX_DACS) {
        dacs[dac_count++] = (uint8_t)dac;
    }
    kprintf("hda: %s (pin 0x%x) <- DAC 0x%x, %d hops\n", device_name(device), pin, dac, len - 1);
    return 0;
}

/* Finds the codec's audio function group and routes every analog output
 * pin it can. Returns how many it routed. */
static int setup_codec(void) {
    command_timeouts = 0;
    uint32_t vendor = param(0, PARAM_VENDOR_ID);
    uint32_t sub = param(0, PARAM_NODE_COUNT);
    afg = 0;
    for (uint32_t nid = (sub >> 16) & 0xFF, end = nid + (sub & 0xFF); nid < end; nid++) {
        if ((param(nid, PARAM_FUNCTION_TYPE) & 0xFF) == 1) {
            afg = nid;
            break;
        }
    }
    if (afg == 0) {
        kprintf("hda: codec %u (%x:%x): no audio function\n", codec, vendor >> 16,
                vendor & 0xFFFF);
        return 0;
    }
    verb(afg, VERB_SET_POWER_STATE, 0);
    timer_sleep_ms(10);
    out_amp_default = param(afg, PARAM_OUT_AMP_CAPS);
    in_amp_default = param(afg, PARAM_IN_AMP_CAPS);

    memset(nodes, 0, sizeof(nodes));
    sub = param(afg, PARAM_NODE_COUNT);
    uint32_t first = (sub >> 16) & 0xFF, count = sub & 0xFF;
    for (uint32_t nid = first; nid < first + count && nid < MAX_NODES; nid++) {
        nodes[nid].caps = param(nid, PARAM_WIDGET_CAPS);
        read_connections(nid);
        if (type_of(nid) == WIDGET_PIN) {
            nodes[nid].pin_caps = param(nid, PARAM_PIN_CAPS);
            nodes[nid].config = verb(nid, VERB_GET_CONFIG_DEFAULT, 0);
        }
    }

    if (command_timeouts > 0) {
        return 0;
    }
    kprintf("hda: codec %u: %x:%x, %u widgets\n", codec, vendor >> 16, vendor & 0xFFFF, count);
    dac_count = 0;
    int routed = 0;
    for (uint32_t nid = first; nid < first + count && nid < MAX_NODES; nid++) {
        struct node *n = &nodes[nid];
        uint32_t device = (n->config >> 20) & 0xF;
        if (type_of(nid) != WIDGET_PIN || (n->config >> 30) == 1 /* nothing attached */ ||
            !(n->pin_caps & PINCAP_OUT) || (n->pin_caps & (PINCAP_HDMI | PINCAP_DP)) ||
            (n->caps & WCAP_DIGITAL) || device > DEVICE_HEADPHONE) {
            continue;
        }
        if (route_pin(nid) == 0) {
            routed++;
        }
    }
    return routed;
}

static int try_controller(const struct pci_device *pci) {
    uint16_t cmd = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
    pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, (uint16_t)(cmd | 0x0406));
    /* TCSEL = 0, as Linux does everywhere: avoids static on some codecs. */
    uint8_t tcsel = pci_config_read8(pci->bus, pci->slot, pci->func, 0x44);
    pci_config_write8(pci->bus, pci->slot, pci->func, 0x44, tcsel & 0xF8);
    if (pci->vendor_id == 0x1022 || pci->vendor_id == 0x1002) {
        /* AMD/ATI: turn on cache snooping (Linux's ATI snoop type). */
        uint8_t misc = pci_config_read8(pci->bus, pci->slot, pci->func, 0x42);
        pci_config_write8(pci->bus, pci->slot, pci->func, 0x42, (uint8_t)((misc & 0xF8) | 0x02));
    }

    uint64_t bar = pci->bar[0] & ~0xFull;
    if ((pci->bar[0] & 0x6) == 0x4) {
        bar |= (uint64_t)pci->bar[1] << 32;
    }
    regs = (volatile uint8_t *)vmm_map_mmio(bar, 0x1000);

    if (reset_controller() != 0) {
        kprintf("hda: %u:%u.%u: controller reset timed out\n", pci->bus, pci->slot, pci->func);
        return -1;
    }
    wr32(INTCTL, 0); /* Polled. */
    uint16_t codecs = rd16(STATESTS);
    wr16(STATESTS, codecs);
    if (codecs == 0) {
        kprintf("hda: %u:%u.%u: no codecs\n", pci->bus, pci->slot, pci->func);
        return -1;
    }
    if (start_rings() != 0) {
        return -1;
    }

    uint16_t gcap = rd16(GCAP);
    uint32_t input_streams = (gcap >> 8) & 0xF;
    if (((gcap >> 12) & 0xF) == 0) {
        kprintf("hda: %u:%u.%u: no output streams\n", pci->bus, pci->slot, pci->func);
        return -1;
    }
    stream_base = SD_BASE(input_streams); /* Output streams come after the inputs. */

    for (codec = 0; codec < 15; codec++) {
        if ((codecs & (1u << codec)) && setup_codec() > 0) {
            return 0;
        }
    }
    kprintf("hda: %u:%u.%u: no analog outputs (HDMI/DP audio needs a GPU driver)\n", pci->bus,
            pci->slot, pci->func);
    return -1;
}

int hda_init(void) {
    for (int i = 0;; i++) {
        const struct pci_device *pci = pci_find_class_nth(0x04, 0x03, 0x00, i);
        if (pci == NULL) {
            break;
        }
        kprintf("hda: controller at %u:%u.%u (%x:%x)\n", pci->bus, pci->slot, pci->func,
                pci->vendor_id, pci->device_id);
        if (try_controller(pci) != 0) {
            if (regs != NULL) {
                wr8(CORBCTL, 0); /* Leave it quiet: no ring DMA left running. */
                wr8(RIRBCTL, 0);
                regs = NULL;
            }
            continue;
        }

        buffer = dma_pages(BUFFER_BYTES / PMM_PAGE_SIZE, &buffer_phys);
        bdl = dma_pages(1, &bdl_phys);
        if (buffer == NULL || bdl == NULL) {
            kprintf("hda: out of memory\n");
            return -1;
        }
        for (int e = 0; e < BDL_ENTRIES; e++) {
            bdl[e].address = buffer_phys + (uint64_t)e * (BUFFER_BYTES / BDL_ENTRIES);
            bdl[e].length = BUFFER_BYTES / BDL_ENTRIES;
            bdl[e].flags = 0; /* No completion interrupts: polled. */
        }
        flush(bdl, sizeof(struct bdl_entry) * BDL_ENTRIES);
        ready = 1;
        return 0;
    }
    return -1;
}

/* --- Playback --- */

static void stop_stream(void) {
    wr8(stream_base + SD_CTL, rd8(stream_base + SD_CTL) & ~SD_CTL_RUN);
    wait8(stream_base + SD_CTL, SD_CTL_RUN, 0, 20);
    running = 0;
}

int hda_open(uint32_t rate_hz, uint8_t channels) {
    if (!ready || (rate_hz != 44100 && rate_hz != 48000) || (channels != 1 && channels != 2)) {
        return -1;
    }
    if (open) {
        stop_stream();
    }

    /* Stream reset (3.3.35). */
    wr8(stream_base + SD_CTL, SD_CTL_SRST);
    wait8(stream_base + SD_CTL, SD_CTL_SRST, SD_CTL_SRST, 20);
    wr8(stream_base + SD_CTL, 0);
    wait8(stream_base + SD_CTL, SD_CTL_SRST, 0, 20);

    /* PCM, 44.1 or 48 kHz base, x1, /1, 16-bit, 2 channels (3.7.1). */
    uint16_t fmt = (uint16_t)((rate_hz == 44100 ? 0x4000 : 0) | (1u << 4) | 1u);
    memset(buffer, 0, BUFFER_BYTES);
    flush(buffer, BUFFER_BYTES);
    wr32(stream_base + SD_BDPL, (uint32_t)bdl_phys);
    wr32(stream_base + SD_BDPU, (uint32_t)(bdl_phys >> 32));
    wr32(stream_base + SD_CBL, BUFFER_BYTES);
    wr16(stream_base + SD_LVI, BDL_ENTRIES - 1);
    wr16(stream_base + SD_FMT, fmt);
    wr8(stream_base + SD_CTL + 2, STREAM_TAG << 4);
    wr8(stream_base + SD_STS, 0x1C); /* Clear any stale status. */

    for (int i = 0; i < dac_count; i++) {
        verb4(dacs[i], VERB4_SET_FORMAT, fmt);
        verb(dacs[i], VERB_SET_STREAM_CHANNEL, STREAM_TAG << 4); /* Channels 0-1. */
    }

    mono = channels == 1;
    written = played = 0;
    last_lpib = 0;
    running = 0;
    open = 1;
    return 0;
}

static void start_stream(void) {
    wr8(stream_base + SD_CTL, rd8(stream_base + SD_CTL) | SD_CTL_RUN);
    running = 1;
}

/* Advances `played` by how far the DMA got, and zeroes what it has
 * finished with -- so if the writer falls behind, the ring wraps into
 * silence rather than replaying old audio. */
static void update_played(void) {
    if (!running) {
        return;
    }
    uint32_t lpib = rd32(stream_base + SD_LPIB) % BUFFER_BYTES;
    uint32_t delta = (lpib + BUFFER_BYTES - last_lpib) % BUFFER_BYTES;
    for (uint32_t i = 0; i < delta;) {
        uint32_t at = (last_lpib + i) % BUFFER_BYTES;
        uint32_t n = BUFFER_BYTES - at < delta - i ? BUFFER_BYTES - at : delta - i;
        memset(buffer + at, 0, n);
        flush(buffer + at, n);
        i += n;
    }
    last_lpib = lpib;
    played += delta;
    if (played > written) {
        written = played; /* Underrun: carry on from where it's playing now. */
    }
}

/* Copies up to `n` bytes of stereo S16LE into the ring; returns how many
 * fit right now. */
static uint32_t put(const uint8_t *src, uint32_t n) {
    uint64_t queued = written - played;
    uint32_t space = queued + GUARD_BYTES >= BUFFER_BYTES
                         ? 0
                         : (uint32_t)(BUFFER_BYTES - GUARD_BYTES - queued);
    if (n > space) {
        n = space & ~3u; /* Whole stereo frames. */
    }
    for (uint32_t done = 0; done < n;) {
        uint32_t at = (uint32_t)(written % BUFFER_BYTES);
        uint32_t chunk = BUFFER_BYTES - at < n - done ? BUFFER_BYTES - at : n - done;
        memcpy(buffer + at, src + done, chunk);
        flush(buffer + at, chunk);
        written += chunk;
        done += chunk;
    }
    return n;
}

/* One idle moment, letting the tick in so the stall timeout below keeps
 * counting inside the syscall. */
static void idle(void) {
    timer_idle();
}

int hda_write(const void *pcm_s16le, uint32_t bytes) {
    if (!open) {
        return -1;
    }
    const uint8_t *src = pcm_s16le;
    uint8_t stereo[1024];
    uint64_t stalled_since = timer_uptime_ms();
    uint64_t last_played = played;

    for (uint32_t done = 0; done < bytes;) {
        /* Mono goes out on both channels, a slice at a time. */
        const uint8_t *chunk = src + done;
        uint32_t in_len = bytes - done, out_len = in_len;
        if (mono) {
            in_len = in_len > sizeof(stereo) / 2 ? sizeof(stereo) / 2 : in_len & ~1u;
            for (uint32_t i = 0; i < in_len; i += 2) {
                stereo[i * 2] = stereo[i * 2 + 2] = chunk[i];
                stereo[i * 2 + 1] = stereo[i * 2 + 3] = chunk[i + 1];
            }
            chunk = stereo;
            out_len = in_len * 2;
            if (in_len == 0) {
                break; /* A stray odd byte. */
            }
        }

        uint32_t out_done = 0;
        while (out_done < out_len) {
            update_played();
            uint32_t n = put(chunk + out_done, out_len - out_done);
            out_done += n;
            if (!running && written - played >= BUFFER_BYTES / 2) {
                start_stream();
            }
            if (n == 0) {
                if (!running) {
                    start_stream();
                }
                if (played != last_played) {
                    last_played = played;
                    stalled_since = timer_uptime_ms();
                } else if (timer_uptime_ms() - stalled_since > 1000) {
                    kprintf("hda: playback stalled -- the stream isn't moving\n");
                    stop_stream();
                    open = 0;
                    return -1;
                }
                idle();
            }
        }
        done += in_len;
    }
    return (int)bytes;
}

int hda_close(void) {
    if (!open) {
        return -1;
    }
    /* 50 ms of silence after the end: stopping the stream the moment the
     * last byte leaves would cut off whatever the codec still holds. */
    static const uint8_t silence[1024];
    uint32_t pad = 48000 / 20 * 4;
    for (uint32_t done = 0; done < pad && open;) {
        uint32_t n = pad - done > sizeof(silence) ? sizeof(silence) : pad - done;
        if (hda_write(silence, mono ? n / 2 : n) < 0) {
            return -1;
        }
        done += n;
    }
    if (!running && written > played) {
        start_stream(); /* Shorter than half the buffer: never got going. */
    }
    uint64_t deadline = timer_uptime_ms() + BUFFER_BYTES * 1000ull / (44100 * 4) + 200;
    while (running && played < written && timer_uptime_ms() < deadline) {
        update_played();
        idle();
    }
    stop_stream();
    open = 0;
    return 0;
}
