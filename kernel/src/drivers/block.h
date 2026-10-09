#ifndef DRIVERS_BLOCK_H
#define DRIVERS_BLOCK_H

#include <stdint.h>

/* Every disk the storage drivers find -- NVMe, SATA (AHCI), USB mass
 * storage, virtio-blk -- behind one interface, addressed in the disk's
 * own logical sectors (512 or 4096 bytes).
 *
 * Writes are locked: block_write() refuses unless the kernel was booted
 * with `allow-disk-writes` on its command line (limine.conf `cmdline:`),
 * which only test configurations pass. The live USB image never does, so
 * nothing it runs can change a disk. virtio-blk, QEMU's own disk image
 * (AnssOS-disk.img), isn't registered here and isn't affected. */

#define BLOCK_MAX_DEVICES 16

struct block_device {
    char name[8];    /* "nvme0", "sata0", "usb0" */
    char model[41];  /* As the drive reports it, trimmed. */
    uint32_t sector_size;
    uint64_t sector_count;
    uint32_t max_sectors; /* Most a single driver call may move. */

    /* `buf` is any kernel memory; drivers bounce through their own DMA
     * memory. Return 0 on success, -1 on failure. */
    int (*read)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
    void *driver;
};

/* Adds a disk (the struct must stay alive). Returns its index or -1. */
int block_register(struct block_device *dev);

int block_count(void);
struct block_device *block_get(int index);
struct block_device *block_find(const char *name);

/* Bounds-checked, split into driver-sized pieces. */
int block_read(struct block_device *dev, uint64_t lba, uint64_t count, void *buf);
int block_write(struct block_device *dev, uint64_t lba, uint64_t count, const void *buf);

/* Reads `allow-disk-writes` from the command line; called once at boot. */
void block_init(void);
int block_writes_allowed(void);

#endif
