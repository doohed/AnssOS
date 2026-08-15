#include "virtio_input.h"
#include "virtio.h"
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
static int initialized; /* Guards virtio_input_poll_char() if init never ran or failed. */

/* US QWERTY: Linux key codes (see the kernel's input-event-codes.h) ->
 * ASCII. 0 means "no mapping, drop the key".
 *
 * Escape (code 1) matters more than it looks: it's the only way out of
 * insert mode in userland/scarf.c, and without it modal editing is
 * impossible on this input path. Over serial the terminal sends 0x1b
 * itself, so this gap only ever showed up in a graphical window. */
static const char KEYMAP_LOWER[62] = {
    [1] = 0x1b,  [2] = '1',  [3] = '2',  [4] = '3',   [5] = '4',  [6] = '5',   [7] = '6',
    [8] = '7',   [9] = '8',  [10] = '9', [11] = '0',  [12] = '-', [13] = '=',  [14] = '\b',
    [15] = '\t', [16] = 'q', [17] = 'w', [18] = 'e',  [19] = 'r', [20] = 't',  [21] = 'y',
    [22] = 'u',  [23] = 'i', [24] = 'o', [25] = 'p',  [26] = '[', [27] = ']',  [28] = '\n',
    [30] = 'a',  [31] = 's', [32] = 'd', [33] = 'f',  [34] = 'g', [35] = 'h',  [36] = 'j',
    [37] = 'k',  [38] = 'l', [39] = ';', [40] = '\'', [41] = '`', [43] = '\\', [44] = 'z',
    [45] = 'x',  [46] = 'c', [47] = 'v', [48] = 'b',  [49] = 'n', [50] = 'm',  [51] = ',',
    [52] = '.',  [53] = '/', [57] = ' ',
};

/* The shifted half of the same layout. Previously shift only uppercased
 * letters, which left every shifted symbol unreachable -- including ':',
 * without which userland/scarf.c has no way to type :w or :q. Codes absent
 * here fall back to KEYMAP_LOWER (with a-z uppercased), so an unshifted
 * key never stops working just because its shifted form is unlisted. */
static const char KEYMAP_UPPER[62] = {
    [2] = '!',  [3] = '@',  [4] = '#',  [5] = '$',  [6] = '%',  [7] = '^',  [8] = '&',
    [9] = '*',  [10] = '(', [11] = ')', [12] = '_', [13] = '+', [26] = '{', [27] = '}',
    [39] = ':', [40] = '"', [41] = '~', [43] = '|', [51] = '<', [52] = '>', [53] = '?',
};

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

        if (ev.value != 1 && ev.value != 2) {
            continue; /* Only care about press (1) and repeat (2), not release (0). */
        }

        if (ev.code >= sizeof(KEYMAP_LOWER) / sizeof(KEYMAP_LOWER[0])) {
            continue;
        }
        char c = KEYMAP_LOWER[ev.code];
        if (shift_held && KEYMAP_UPPER[ev.code] != '\0') {
            c = KEYMAP_UPPER[ev.code];
        } else if (shift_held && c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c == '\0') {
            continue;
        }
        return (unsigned char)c;
    }
}
