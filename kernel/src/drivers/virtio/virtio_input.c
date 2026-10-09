#include "virtio_input.h"
#include "virtio.h"
#include "../keymap.h"
#include "../pci.h"
#include "../serial.h"
#include "../../boot/requests.h"
#include "../../lib/string.h"
#include "../../mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

#define VIRTIO_INPUT_PCI_VENDOR_ID 0x1af4
#define VIRTIO_INPUT_PCI_DEVICE_ID 0x1052

#define VIRTIO_INPUT_CFG_ID_NAME 0x01

#define EV_KEY 1

#define KEY_LEFTSHIFT 42
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTCTRL 29
#define KEY_RIGHTCTRL 97

/* Layout mandated by the virtio spec ("Input configuration layout").
 * `select`/`subsel` bank-switch which member of the union is currently
 * readable -- see virtio_input_read_name() below. */
struct __attribute__((packed)) virtio_input_config {
    uint8_t select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    union {
        char string[128];
        uint8_t bitmap[128];
        struct {
            uint32_t min, max, fuzz, flat, res;
        } abs;
        struct {
            uint16_t bustype, vendor, product, version;
        } ids;
    } u;
};

struct __attribute__((packed)) virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

#define EVENTQ_INDEX 0
#define EVENTQ_BUFFERS 64

static struct virtio_device vdev;
static struct virtio_queue eventq;
static struct virtio_input_event *event_bufs; /* HHDM-mapped, EVENTQ_BUFFERS entries. */

static int shift_held;
static int ctrl_held;
static int initialized; /* Guards virtio_input_poll_char() if init never ran or failed. */

/* A key's bytes come from keymap_bytes() (drivers/keymap.c); this
 * driver hands out one byte per poll, so a multi-byte escape sequence
 * returns its first byte and parks the rest here for the next polls. */
static const char *pending_seq;

static char lower_char(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static int contains_ci(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] != '\0' && lower_char(p[i]) == lower_char(needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return 1;
        }
    }
    return 0;
}

/* Reads the device's ID_NAME string via the select/subsel bank-switch --
 * write which field you want, then read it back out of the union. */
static void read_device_name(volatile struct virtio_input_config *cfg, char *out, size_t out_size) {
    cfg->select = VIRTIO_INPUT_CFG_ID_NAME;
    cfg->subsel = 0;

    uint8_t size = cfg->size;
    if (size > out_size - 1) {
        size = (uint8_t)(out_size - 1);
    }
    for (uint8_t i = 0; i < size; i++) {
        out[i] = cfg->u.string[i];
    }
    out[size] = '\0';
}

static void post_buffer(uint16_t index) {
    struct virtio_buffer buf = {
        .addr = &event_bufs[index],
        .len = sizeof(struct virtio_input_event),
        .device_writable = 1,
    };
    virtio_queue_submit_chain(&eventq, &buf, 1);
}

int virtio_input_init(void) {
    const struct pci_device *pci = NULL;

    for (int i = 0;; i++) {
        const struct pci_device *candidate_pci =
            pci_find_device_nth(VIRTIO_INPUT_PCI_VENDOR_ID, VIRTIO_INPUT_PCI_DEVICE_ID, i);
        if (candidate_pci == NULL) {
            break;
        }

        struct virtio_device candidate;
        if (virtio_pci_init(candidate_pci, &candidate) != 0 || candidate.device_cfg == NULL) {
            continue;
        }

        char name[65];
        read_device_name((volatile struct virtio_input_config *)candidate.device_cfg, name,
                         sizeof(name));
        kprintf("virtio-input: found \"%s\" at %u:%u.%u\n", name, candidate_pci->bus,
                candidate_pci->slot, candidate_pci->func);

        if (contains_ci(name, "keyboard")) {
            vdev = candidate;
            pci = candidate_pci;
            break;
        }
    }

    if (pci == NULL) {
        kprintf(
            "virtio-input: no keyboard device found -- boot QEMU with "
            "-device virtio-keyboard-pci\n");
        return -1;
    }

    if (virtio_negotiate_features(&vdev, 0) != 0) {
        return -1;
    }
    if (virtio_queue_init(&vdev, EVENTQ_INDEX, &eventq) != 0) {
        return -1;
    }
    virtio_driver_ok(&vdev);

    uint64_t bufs_phys = pmm_alloc_page();
    if (bufs_phys == 0) {
        kprintf("virtio-input: out of memory\n");
        return -1;
    }
    event_bufs =
        (struct virtio_input_event *)(uintptr_t)(bufs_phys + hhdm_request.response->offset);
    memset(event_bufs, 0, PMM_PAGE_SIZE);

    uint32_t count = EVENTQ_BUFFERS;
    if (count > eventq.size) {
        count = eventq.size;
    }
    for (uint16_t i = 0; i < count; i++) {
        post_buffer(i);
    }

    initialized = 1;
    kprintf("virtio-input: keyboard ready (%u event buffers posted)\n", count);
    return 0;
}

int virtio_input_poll_char(void) {
    if (!initialized) {
        return -1;
    }
    if (pending_seq != NULL) {
        char c = *pending_seq++;
        if (*pending_seq == '\0') {
            pending_seq = NULL;
        }
        return (unsigned char)c;
    }

    for (;;) {
        uint16_t desc_id;
        uint32_t len;
        if (!virtio_queue_try_wait(&eventq, &desc_id, &len)) {
            return -1;
        }

        struct virtio_input_event ev = event_bufs[desc_id];
        post_buffer(desc_id); /* Same slot the device just filled -- repost immediately. */
        (void)len;            /* Always sizeof(struct virtio_input_event); not interesting. */

        if (ev.type != EV_KEY) {
            continue; /* EV_SYN (frame separators) etc. -- not interesting here. */
        }

        if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
            shift_held = (ev.value != 0);
            continue;
        }

        /* Tracked like shift. Needed for scarf's vim window commands,
         * which are all Ctrl-w prefixed -- over serial the terminal
         * produces the 0x17 control byte itself, so this only ever
         * mattered in a graphical window. */
        if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
            ctrl_held = (ev.value != 0);
            continue;
        }

        if (ev.value != 1 && ev.value != 2) {
            continue; /* Only care about press (1) and repeat (2), not release (0). */
        }

        const char *bytes = keymap_bytes(ev.code, (shift_held ? KEYMOD_SHIFT : 0) |
                                                      (ctrl_held ? KEYMOD_CTRL : 0));
        if (bytes == NULL) {
            continue;
        }
        if (bytes[1] != '\0') {
            pending_seq = bytes + 1;
        }
        return (unsigned char)bytes[0];
    }
}
