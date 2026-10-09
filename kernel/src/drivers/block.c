#include "block.h"
#include "serial.h"
#include "../boot/requests.h"
#include "../lib/string.h"

#include <stddef.h>
#include <stdint.h>

static struct block_device *devices[BLOCK_MAX_DEVICES];
static int device_count;
static int writes_allowed;

void block_init(void) {
    writes_allowed = boot_option("allow-disk-writes");
    if (writes_allowed) {
        kprintf("block: disk writes ALLOWED (allow-disk-writes on the command line)\n");
    }
}

int block_writes_allowed(void) {
    return writes_allowed;
}

int block_register(struct block_device *dev) {
    if (device_count == BLOCK_MAX_DEVICES) {
        return -1;
    }
    devices[device_count] = dev;
    return device_count++;
}

int block_count(void) {
    return device_count;
}

struct block_device *block_get(int index) {
    return index >= 0 && index < device_count ? devices[index] : NULL;
}

struct block_device *block_find(const char *name) {
    for (int i = 0; i < device_count; i++) {
        if (strcmp(devices[i]->name, name) == 0) {
            return devices[i];
        }
    }
    return NULL;
}

static int in_range(struct block_device *dev, uint64_t lba, uint64_t count) {
    return count > 0 && lba < dev->sector_count && count <= dev->sector_count - lba;
}

int block_read(struct block_device *dev, uint64_t lba, uint64_t count, void *buf) {
    if (!in_range(dev, lba, count)) {
        return -1;
    }
    uint8_t *p = buf;
    while (count > 0) {
        uint32_t n = count > dev->max_sectors ? dev->max_sectors : (uint32_t)count;
        if (dev->read(dev, lba, n, p) != 0) {
            return -1;
        }
        lba += n;
        count -= n;
        p += (uint64_t)n * dev->sector_size;
    }
    return 0;
}

int block_write(struct block_device *dev, uint64_t lba, uint64_t count, const void *buf) {
    if (!writes_allowed) {
        kprintf("block: refusing to write to %s -- disk writes are locked\n", dev->name);
        return -1;
    }
    if (!in_range(dev, lba, count) || dev->write == NULL) {
        return -1;
    }
    const uint8_t *p = buf;
    while (count > 0) {
        uint32_t n = count > dev->max_sectors ? dev->max_sectors : (uint32_t)count;
        if (dev->write(dev, lba, n, p) != 0) {
            return -1;
        }
        lba += n;
        count -= n;
        p += (uint64_t)n * dev->sector_size;
    }
    return 0;
}
