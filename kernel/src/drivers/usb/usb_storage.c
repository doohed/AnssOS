#include "usb_storage.h"
#include "../block.h"
#include "../serial.h"
#include "../timer.h"
#include "../../boot/requests.h"
#include "../../lib/string.h"
#include "../../mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

/* USB Mass Storage Class, Bulk-Only Transport (rev 1.0): every command is
 * a 31-byte Command Block Wrapper out, the data, then a 13-byte Command
 * Status Wrapper in. The commands themselves are SCSI (SBC-3/SPC-4). */

#define MAX_DISKS 4
#define BOUNCE_BYTES (64 * 1024)
#define CBW_SIGNATURE 0x43425355u /* "USBC" */
#define CSW_SIGNATURE 0x53425355u /* "USBS" */
#define TRANSFER_TIMEOUT_MS 10000

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2A
#define SCSI_READ_16 0x88
#define SCSI_WRITE_16 0x8A
#define SCSI_READ_CAPACITY_16 0x9E

struct __attribute__((packed)) cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t flags; /* 0x80: data comes in */
    uint8_t lun;
    uint8_t cb_length;
    uint8_t cb[16];
};

struct __attribute__((packed)) csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status; /* 0 passed, 1 failed, 2 phase error */
};

struct usb_disk {
    struct usb_device *dev;
    uint8_t interface, ep_in, ep_out;
    int dci_in, dci_out;
    uint32_t tag;
    uint8_t *bounce; /* BOUNCE_BYTES of data, then a page for CBW/CSW. */
    uint64_t bounce_phys;
    struct block_device block;
};

static struct usb_disk disks[MAX_DISKS];
static int disk_count;

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p) {
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

/* Reset Recovery (BOT 5.3.4): reset the interface, then clear the halt on
 * both pipes -- what's needed after a phase error or a transfer gone
 * wrong to get the device listening for commands again. */
static void reset_recovery(struct usb_disk *d) {
    usb_control(d->dev, 0x21, 0xFF, 0, d->interface, NULL, 0); /* Bulk-Only Mass Storage Reset */
    usb_control(d->dev, 0x02, 0x01, 0, d->ep_in, NULL, 0);     /* CLEAR_FEATURE(ENDPOINT_HALT) */
    usb_control(d->dev, 0x02, 0x01, 0, d->ep_out, NULL, 0);
}

/* One SCSI command. Data moves through the bounce buffer: `in` says
 * which way. Returns 0 if the device reports success. */
static int scsi(struct usb_disk *d, const uint8_t *cb, uint8_t cb_length, uint32_t length, int in) {
    uint8_t *small = d->bounce + BOUNCE_BYTES;
    uint64_t small_phys = d->bounce_phys + BOUNCE_BYTES;
    struct cbw *w = (struct cbw *)small;
    memset(w, 0, sizeof(*w));
    w->signature = CBW_SIGNATURE;
    w->tag = ++d->tag;
    w->data_length = length;
    w->flags = in ? 0x80 : 0;
    w->cb_length = cb_length;
    memcpy(w->cb, cb, cb_length);

    uint32_t moved;
    uint32_t cc = usb_bulk(d->dev, d->dci_out, small_phys, sizeof(*w), &moved, TRANSFER_TIMEOUT_MS);
    if (cc != 1) {
        reset_recovery(d);
        return -1;
    }
    if (length > 0) {
        cc = usb_bulk(d->dev, in ? d->dci_in : d->dci_out, d->bounce_phys, length, &moved,
                      TRANSFER_TIMEOUT_MS);
        if (cc == 6) {
            /* Stalled data stage: clear it on the device; the CSW follows. */
            usb_control(d->dev, 0x02, 0x01, 0, in ? d->ep_in : d->ep_out, NULL, 0);
        } else if (cc != 1 && cc != 13) {
            reset_recovery(d);
            return -1;
        }
    }

    struct csw *s = (struct csw *)(small + 64);
    uint64_t s_phys = small_phys + 64;
    memset(s, 0, sizeof(*s));
    cc = usb_bulk(d->dev, d->dci_in, s_phys, sizeof(*s), &moved, TRANSFER_TIMEOUT_MS);
    if (cc == 6) {
        usb_control(d->dev, 0x02, 0x01, 0, d->ep_in, NULL, 0);
        cc = usb_bulk(d->dev, d->dci_in, s_phys, sizeof(*s), &moved, TRANSFER_TIMEOUT_MS);
    }
    if ((cc != 1 && cc != 13) || s->signature != CSW_SIGNATURE || s->tag != w->tag ||
        s->status == 2) {
        reset_recovery(d);
        return -1;
    }
    return s->status == 0 ? 0 : -1;
}

static int rw(struct block_device *dev, uint64_t lba, uint32_t count, int write) {
    struct usb_disk *d = dev->driver;
    uint8_t cb[16];
    memset(cb, 0, sizeof(cb));
    uint8_t len;
    if (lba + count <= 0xFFFFFFFFull) {
        cb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
        put_be32(cb + 2, (uint32_t)lba);
        cb[7] = (uint8_t)(count >> 8);
        cb[8] = (uint8_t)count;
        len = 10;
    } else {
        cb[0] = write ? SCSI_WRITE_16 : SCSI_READ_16;
        put_be64(cb + 2, lba);
        put_be32(cb + 10, count);
        len = 16;
    }
    if (scsi(d, cb, len, count * dev->sector_size, !write) != 0) {
        kprintf("usb-storage: %s: %s of %u sectors at %lu failed\n", dev->name,
                write ? "write" : "read", count, lba);
        return -1;
    }
    return 0;
}

static int storage_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    struct usb_disk *d = dev->driver;
    if (rw(dev, lba, count, 0) != 0) {
        return -1;
    }
    memcpy(buf, d->bounce, (size_t)count * dev->sector_size);
    return 0;
}

static int storage_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    struct usb_disk *d = dev->driver;
    memcpy(d->bounce, buf, (size_t)count * dev->sector_size);
    return rw(dev, lba, count, 1);
}

static void trimmed(char *out, const uint8_t *field, int len) {
    int n = len;
    while (n > 0 && (field[n - 1] == ' ' || field[n - 1] == 0)) {
        n--;
    }
    memcpy(out, field, (size_t)n);
    out[n] = '\0';
}

/* Asks for the medium until it's ready -- sticks and card readers can
 * take a moment after being configured. */
static int wait_ready(struct usb_disk *d) {
    uint8_t cb[16];
    for (int tries = 0; tries < 20; tries++) {
        memset(cb, 0, sizeof(cb));
        cb[0] = SCSI_TEST_UNIT_READY;
        if (scsi(d, cb, 6, 0, 0) == 0) {
            return 0;
        }
        memset(cb, 0, sizeof(cb));
        cb[0] = SCSI_REQUEST_SENSE; /* Clears the "unit attention" a fresh device reports. */
        cb[4] = 18;
        scsi(d, cb, 6, 18, 1);
        timer_sleep_ms(100);
    }
    return -1;
}

static int start(struct usb_disk *d) {
    uint8_t cb[16];
    memset(cb, 0, sizeof(cb));
    cb[0] = SCSI_INQUIRY;
    cb[4] = 36;
    if (scsi(d, cb, 6, 36, 1) != 0) {
        kprintf("usb-storage: port %u: INQUIRY failed\n", d->dev->port);
        return -1;
    }
    if ((d->bounce[0] & 0x1F) != 0x00) {
        kprintf("usb-storage: port %u: not a disk (SCSI type %u) -- skipped\n", d->dev->port,
                d->bounce[0] & 0x1F);
        return -1;
    }
    char vendor[9], product[17];
    trimmed(vendor, d->bounce + 8, 8);
    trimmed(product, d->bounce + 16, 16);
    size_t vl = strlen(vendor);
    memcpy(d->block.model, vendor, vl);
    d->block.model[vl] = ' ';
    memcpy(d->block.model + vl + 1, product, strlen(product) + 1);

    if (wait_ready(d) != 0) {
        kprintf("usb-storage: port %u: no medium, or it never became ready\n", d->dev->port);
        return -1;
    }

    memset(cb, 0, sizeof(cb));
    cb[0] = SCSI_READ_CAPACITY_10;
    if (scsi(d, cb, 10, 8, 1) != 0) {
        kprintf("usb-storage: port %u: READ CAPACITY failed\n", d->dev->port);
        return -1;
    }
    uint64_t last = be32(d->bounce);
    uint32_t sector_size = be32(d->bounce + 4);
    if (last == 0xFFFFFFFFu) { /* Over 2 TiB: ask again with 64-bit LBAs. */
        memset(cb, 0, sizeof(cb));
        cb[0] = SCSI_READ_CAPACITY_16;
        cb[1] = 0x10;
        cb[13] = 32;
        if (scsi(d, cb, 16, 32, 1) != 0) {
            return -1;
        }
        last = be64(d->bounce);
        sector_size = be32(d->bounce + 8);
    }
    if (sector_size != 512 && sector_size != 2048 && sector_size != 4096) {
        kprintf("usb-storage: port %u: unusable sector size %u\n", d->dev->port, sector_size);
        return -1;
    }
    d->block.sector_size = sector_size;
    d->block.sector_count = last + 1;
    d->block.max_sectors = BOUNCE_BYTES / sector_size;
    d->block.read = storage_read;
    d->block.write = storage_write;
    d->block.driver = d;
    return 0;
}

int usb_storage_probe(struct usb_device *dev, const uint8_t *config, uint16_t length) {
    if (disk_count == MAX_DISKS) {
        return -1;
    }
    /* A mass-storage interface (class 8) speaking SCSI (subclass 6) over
     * Bulk-Only Transport (protocol 0x50), and its two bulk endpoints. */
    int in_storage = 0;
    uint8_t interface = 0, ep_in = 0, ep_out = 0;
    uint16_t mps_in = 0, mps_out = 0;
    for (uint16_t off = 0; off + 2 <= length && config[off] >= 2; off += config[off]) {
        const uint8_t *desc = config + off;
        if (off + desc[0] > length) {
            break;
        }
        if (desc[1] == 4 && desc[0] >= 9) {
            if (ep_in && ep_out) {
                break;
            }
            in_storage = desc[5] == 8 && desc[6] == 6 && desc[7] == 0x50;
            interface = desc[2];
        } else if (desc[1] == 5 && desc[0] >= 7 && in_storage && (desc[3] & 3) == 2) {
            uint16_t mps = (uint16_t)((desc[4] | (desc[5] << 8)) & 0x7FF);
            if (desc[2] & 0x80) {
                ep_in = desc[2];
                mps_in = mps;
            } else {
                ep_out = desc[2];
                mps_out = mps;
            }
        }
    }
    if (!ep_in || !ep_out) {
        return -1;
    }

    struct usb_disk *d = &disks[disk_count];
    memset(d, 0, sizeof(*d));
    d->dev = dev;
    d->interface = interface;
    d->ep_in = ep_in;
    d->ep_out = ep_out;
    if (usb_control(dev, 0x00, 9, config[5], 0, NULL, 0) != 0) { /* SET_CONFIGURATION */
        kprintf("usb-storage: port %u: SET_CONFIGURATION failed\n", dev->port);
        return -1;
    }
    d->dci_in = usb_add_bulk(dev, ep_in, mps_in);
    d->dci_out = usb_add_bulk(dev, ep_out, mps_out);
    uint64_t pages = BOUNCE_BYTES / PMM_PAGE_SIZE + 1;
    d->bounce_phys = pmm_alloc_pages(pages);
    if (d->dci_in < 0 || d->dci_out < 0 || d->bounce_phys == 0) {
        return -1;
    }
    d->bounce = (uint8_t *)(uintptr_t)(d->bounce_phys + hhdm_request.response->offset);
    memset(d->bounce, 0, pages * PMM_PAGE_SIZE);
    dev->driver_data = d;

    if (start(d) != 0) {
        return -1;
    }
    d->block.name[0] = 'u';
    d->block.name[1] = 's';
    d->block.name[2] = 'b';
    d->block.name[3] = (char)('0' + disk_count);
    d->block.name[4] = '\0';
    block_register(&d->block);
    disk_count++;
    kprintf("usb-storage: port %u: %s registered as %s\n", dev->port, d->block.model,
            d->block.name);
    return 0;
}
