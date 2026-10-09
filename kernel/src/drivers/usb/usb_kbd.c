#include "usb_kbd.h"
#include "../keymap.h"
#include "../serial.h"
#include "../timer.h"
#include "../../boot/requests.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_KEYBOARDS 4
#define REPORTS_QUEUED 8  /* Transfers kept in flight per keyboard. */
#define REPORT_STRIDE 64  /* Bytes between the report buffers in their page. */
#define REPEAT_DELAY_MS 500
#define REPEAT_RATE_MS 33 /* About 30 a second. */

#define HID_SET_IDLE 0x0A
#define HID_SET_PROTOCOL 0x0B
#define HID_USAGE_CAPS_LOCK 57
#define HID_ERROR_ROLLOVER 1 /* "Too many keys held" -- the report means nothing. */

#define MOD_CTRL 0x11  /* Left | right. */
#define MOD_SHIFT 0x22

struct keyboard {
    struct usb_device *dev; /* NULL: free slot. */
    int dci;
    uint8_t *reports; /* One page, REPORTS_QUEUED buffers REPORT_STRIDE apart. */
    uint64_t reports_phys;
    uint8_t last[8]; /* Previous report: what's held now. */
};

static struct keyboard keyboards[MAX_KEYBOARDS];
static int caps_lock;

/* Repeat: the most recently pressed key that's still held, and where. */
static struct keyboard *repeat_kb;
static uint8_t repeat_usage;
static uint8_t repeat_mods;
static uint64_t repeat_at_ms;

/* Bytes typed but not yet read. */
static char queue[64];
static uint32_t queue_head, queue_tail;

/* HID usage IDs (the USB HID Usage Tables, keyboard page) -> Linux key
 * codes, which is what drivers/keymap.c speaks. Same table as Linux's own
 * usbkbd.c; past 100 is nothing a terminal has a byte for. */
static const uint8_t HID_TO_KEYCODE[101] = {
    0,  0,  0,  0,  30, 48, 46, 32,  18,  33,  34,  35,  23,  36,  37,  38,  50,
    49, 24, 25, 16, 19, 31, 20, 22,  47,  17,  45,  21,  44,  2,   3,   4,   5,
    6,  7,  8,  9,  10, 11, 28, 1,   14,  15,  57,  12,  13,  26,  27,  43,  43,
    39, 40, 41, 51, 52, 53, 58, 59,  60,  61,  62,  63,  64,  65,  66,  67,  68,
    87, 88, 99, 70, 119, 110, 102, 104, 111, 107, 109, 106, 105, 108, 103, 69, 98,
    55, 74, 78, 96, 79, 80,  81,  75,  76,  77,  71,  72,  73,  82,  83,  86,
};

static void push_bytes(const char *s) {
    for (; *s; s++) {
        uint32_t next = (queue_head + 1) % sizeof(queue);
        if (next == queue_tail) {
            return; /* Full -- nobody's reading; drop the rest. */
        }
        queue[queue_head] = *s;
        queue_head = next;
    }
}

static void type_key(uint8_t usage, uint8_t mods) {
    if (usage >= sizeof(HID_TO_KEYCODE)) {
        return;
    }
    int m = ((mods & MOD_SHIFT) ? KEYMOD_SHIFT : 0) | ((mods & MOD_CTRL) ? KEYMOD_CTRL : 0) |
            (caps_lock ? KEYMOD_CAPS : 0);
    const char *bytes = keymap_bytes(HID_TO_KEYCODE[usage], m);
    if (bytes != NULL) {
        push_bytes(bytes);
    }
}

static int held(const uint8_t *report, uint8_t usage) {
    for (int i = 2; i < 8; i++) {
        if (report[i] == usage) {
            return 1;
        }
    }
    return 0;
}

static void handle_report(struct keyboard *kb, const uint8_t *r) {
    if (r[2] == HID_ERROR_ROLLOVER) {
        return;
    }
    for (int i = 2; i < 8; i++) {
        uint8_t usage = r[i];
        if (usage == 0 || held(kb->last, usage)) {
            continue; /* Nothing, or still held from before. */
        }
        if (usage == HID_USAGE_CAPS_LOCK) {
            caps_lock = !caps_lock;
            continue;
        }
        type_key(usage, r[0]);
        repeat_kb = kb;
        repeat_usage = usage;
        repeat_at_ms = timer_uptime_ms() + REPEAT_DELAY_MS;
    }
    if (repeat_kb == kb) {
        if (repeat_usage != 0 && !held(r, repeat_usage)) {
            repeat_usage = 0;
        }
        repeat_mods = r[0];
    }
    memcpy(kb->last, r, sizeof(kb->last));
}

static void on_report(struct usb_device *dev, uint64_t buf_phys, uint32_t len, uint32_t cc) {
    struct keyboard *kb = dev->driver_data;
    if (cc != 1 && cc != 13) {
        /* Most likely unplugged; anything else would need the endpoint
         * reset, and a keyboard that stopped is better than a loop of
         * failing transfers. */
        kprintf("usb-kbd: port %u: transfer failed (%u)\n", dev->port, cc);
        if (repeat_kb == kb) {
            repeat_usage = 0;
        }
        return;
    }
    if (len >= 3) {
        uint8_t report[8] = {0};
        const uint8_t *buf = (const uint8_t *)(uintptr_t)(buf_phys + hhdm_request.response->offset);
        memcpy(report, buf, len < 8 ? len : 8);
        handle_report(kb, report);
    }
    if (!dev->gone) {
        usb_queue_in(dev, kb->dci, buf_phys, 8); /* Reuse the buffer for the next report. */
    }
}

int usb_kbd_probe(struct usb_device *dev, const uint8_t *config, uint16_t length) {
    struct keyboard *kb = NULL;
    for (int i = 0; i < MAX_KEYBOARDS && kb == NULL; i++) {
        if (keyboards[i].dev == NULL || keyboards[i].dev->gone) {
            kb = &keyboards[i]; /* Never used, or its keyboard was unplugged. */
        }
    }
    if (kb == NULL) {
        return -1;
    }

    /* Find a HID boot keyboard interface (class 3, subclass 1, protocol
     * 1) and its interrupt IN endpoint. */
    int in_keyboard = 0;
    uint8_t interface = 0, ep_address = 0, interval = 0;
    uint16_t max_packet = 0;
    for (uint16_t off = 0; off + 2 <= length && config[off] >= 2; off += config[off]) {
        const uint8_t *d = config + off;
        if (off + d[0] > length) {
            break;
        }
        if (d[1] == 4 && d[0] >= 9) { /* Interface */
            in_keyboard = d[5] == 3 && d[6] == 1 && d[7] == 1;
            interface = d[2];
        } else if (d[1] == 5 && d[0] >= 7 && in_keyboard) { /* Endpoint */
            if ((d[2] & 0x80) && (d[3] & 0x3) == 3) {
                ep_address = d[2];
                max_packet = (uint16_t)((d[4] | (d[5] << 8)) & 0x7FF);
                interval = d[6];
                break;
            }
        }
    }
    if (ep_address == 0) {
        return -1;
    }

    if (repeat_kb == kb) {
        repeat_kb = NULL;
        repeat_usage = 0;
    }
    memset(kb, 0, sizeof(*kb));
    dev->driver_data = kb;

    uint8_t config_value = config[5];
    if (usb_control(dev, 0x00, 9, config_value, 0, NULL, 0) != 0) { /* SET_CONFIGURATION */
        kprintf("usb-kbd: port %u: SET_CONFIGURATION failed\n", dev->port);
        return -1;
    }
    if (usb_control(dev, 0x21, HID_SET_PROTOCOL, 0, interface, NULL, 0) != 0) {
        kprintf("usb-kbd: port %u: can't switch to the boot protocol\n", dev->port);
        return -1;
    }
    /* Report only on change; repeat is ours. Optional -- some keyboards
     * refuse it, which is fine. */
    usb_control(dev, 0x21, HID_SET_IDLE, 0, interface, NULL, 0);

    kb->dci = usb_add_interrupt_in(dev, ep_address, max_packet, interval, on_report);
    if (kb->dci < 0) {
        return -1;
    }
    kb->reports_phys = pmm_alloc_page();
    if (kb->reports_phys == 0) {
        return -1;
    }
    kb->reports = (uint8_t *)(uintptr_t)(kb->reports_phys + hhdm_request.response->offset);
    memset(kb->reports, 0, PMM_PAGE_SIZE);
    for (int i = 0; i < REPORTS_QUEUED; i++) {
        usb_queue_in(dev, kb->dci, kb->reports_phys + (uint64_t)i * REPORT_STRIDE, 8);
    }

    kb->dev = dev;
    kprintf("usb-kbd: port %u: keyboard ready\n", dev->port);
    return 0;
}

int usb_kbd_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        n += keyboards[i].dev != NULL && !keyboards[i].dev->gone;
    }
    return n;
}

int usb_kbd_poll_char(void) {
    usb_poll(); /* Cheap with nothing pending; also notices hot-plugged keyboards. */

    if (repeat_usage != 0 && repeat_kb->dev->gone) {
        repeat_usage = 0; /* Unplugged with the key down. */
    }
    if (repeat_usage != 0 && queue_head == queue_tail) {
        uint64_t now = timer_uptime_ms();
        if (now >= repeat_at_ms) {
            type_key(repeat_usage, repeat_mods);
            repeat_at_ms = now + REPEAT_RATE_MS;
        }
    }

    if (queue_head == queue_tail) {
        return -1;
    }
    char c = queue[queue_tail];
    queue_tail = (queue_tail + 1) % sizeof(queue);
    return (unsigned char)c;
}
