#ifndef DRIVERS_VIRTIO_VIRTIO_BLK_H
#define DRIVERS_VIRTIO_VIRTIO_BLK_H

#include <stdint.h>

#define VIRTIO_BLK_PCI_VENDOR_ID 0x1af4
#define VIRTIO_BLK_PCI_DEVICE_ID 0x1042

#define VIRTIO_BLK_SECTOR_SIZE 512

/* Finds the virtio-blk-pci device via PCI (pci_enumerate() must have run
 * already), completes the virtio init handshake, and reads the device's
 * capacity. Returns 0 on success, -1 if no virtio-blk device is present
 * (a perfectly normal, non-fatal configuration -- callers should fall
 * back to in-memory-only behavior, same as virtio-gpu/virtio-input). */
int virtio_blk_init(void);

/* Whether virtio_blk_init() has succeeded -- lets a caller (fs/blkfs.c)
 * tell "no disk attached" (expected, not an error) apart from a real
 * I/O failure without virtio_blk_read()/_write() needing to distinguish
 * the two in their return value. */
int virtio_blk_is_ready(void);

uint64_t virtio_blk_capacity_sectors(void);

/* `buf`/`data` should be HHDM-mapped memory (e.g. from kmalloc(), see
 * mm/heap.h) -- the device DMAs by physical address. Both return 0 on
 * success, -1 on failure (including "virtio-blk was never initialized"). */
int virtio_blk_read(uint64_t sector, void *buf, uint32_t sector_count);
int virtio_blk_write(uint64_t sector, const void *data, uint32_t sector_count);

#endif
