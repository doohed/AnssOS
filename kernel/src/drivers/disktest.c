#include "disktest.h"
#include "serial.h"
#include "timer.h"
#include "../lib/string.h"
#include "../mm/heap.h"

#include <stddef.h>
#include <stdint.h>

#define CHUNK (1024 * 1024)
#define SPEED_BYTES (64ull * 1024 * 1024)

static int read_check(struct block_device *dev, uint8_t *a, uint8_t *b) {
    uint64_t sectors = CHUNK / dev->sector_size;
    if (sectors > dev->sector_count) {
        sectors = dev->sector_count;
    }
    if (block_read(dev, 0, sectors, a) != 0 || block_read(dev, 0, sectors, b) != 0) {
        kprintf("disktest: %s: read failed\n", dev->name);
        return -1;
    }
    if (memcmp(a, b, sectors * dev->sector_size) != 0) {
        kprintf("disktest: %s: two reads of the same sectors DIFFER\n", dev->name);
        return -1;
    }
    kprintf("disktest: %s: first %lu KiB read twice, identical\n", dev->name,
            sectors * dev->sector_size / 1024);

    uint64_t total = dev->sector_count * dev->sector_size;
    uint64_t bytes = total < SPEED_BYTES ? total - total % CHUNK : SPEED_BYTES;
    if (bytes == 0) {
        return 0;
    }
    uint64_t start = timer_uptime_ms();
    for (uint64_t done = 0; done < bytes; done += CHUNK) {
        if (block_read(dev, done / dev->sector_size, CHUNK / dev->sector_size, a) != 0) {
            kprintf("disktest: %s: read failed at byte %lu\n", dev->name, done);
            return -1;
        }
    }
    uint64_t ms = timer_uptime_ms() - start;
    kprintf("disktest: %s: read %lu MiB in %lu ms", dev->name, bytes >> 20, ms);
    if (ms > 0) {
        kprintf(" (%lu MiB/s)", (bytes >> 20) * 1000 / ms);
    }
    kprintf("\n");
    return 0;
}

static int write_check(struct block_device *dev, uint8_t *saved, uint8_t *buf) {
    uint64_t sectors = CHUNK / dev->sector_size;
    if (dev->sector_count < 4 * sectors) {
        kprintf("disktest: %s: too small for the write check\n", dev->name);
        return -1;
    }
    uint64_t lba = (dev->sector_count / 2) & ~(sectors - 1);
    if (block_read(dev, lba, sectors, saved) != 0) {
        kprintf("disktest: %s: reading the region to save it failed\n", dev->name);
        return -1;
    }
    for (uint64_t i = 0; i < CHUNK; i++) {
        buf[i] = (uint8_t)(i * 7 + (i >> 9) + 0x5A);
    }
    if (block_write(dev, lba, sectors, buf) != 0) {
        kprintf("disktest: %s: write failed\n", dev->name);
        return -1;
    }
    memset(buf, 0, CHUNK);
    int ok = block_read(dev, lba, sectors, buf) == 0;
    for (uint64_t i = 0; ok && i < CHUNK; i++) {
        ok = buf[i] == (uint8_t)(i * 7 + (i >> 9) + 0x5A);
    }
    kprintf("disktest: %s: wrote a 1 MiB pattern at sector %lu, read back %s\n", dev->name, lba,
            ok ? "identical" : "DIFFERENT");

    int restored = block_write(dev, lba, sectors, saved) == 0 &&
                   block_read(dev, lba, sectors, buf) == 0 && memcmp(buf, saved, CHUNK) == 0;
    kprintf("disktest: %s: original data %s\n", dev->name,
            restored ? "restored and verified" : "NOT RESTORED");
    return ok && restored ? 0 : -1;
}

int disktest_run(struct block_device *dev, int write) {
    uint8_t *a = kmalloc(CHUNK);
    uint8_t *b = kmalloc(CHUNK);
    int result = -1;
    if (a == NULL || b == NULL) {
        kprintf("disktest: out of memory\n");
    } else if (write) {
        result = block_writes_allowed() ? write_check(dev, a, b) : -1;
        if (!block_writes_allowed()) {
            kprintf("disktest: writes are locked (boot with allow-disk-writes to test them)\n");
        }
    } else {
        result = read_check(dev, a, b);
    }
    kfree(a);
    kfree(b);
    return result;
}
