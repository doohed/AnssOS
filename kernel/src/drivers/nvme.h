#ifndef DRIVERS_NVME_H
#define DRIVERS_NVME_H

/* NVMe SSDs (PCI class 01.08, prog-if 02; QEMU: -device nvme). Brings
 * each controller up with one I/O queue pair, polled, and registers
 * namespace 1 as a block device (drivers/block.h) named nvme0, nvme1...
 * Bringing a controller up only resets it and asks it questions; the
 * drive's data is only touched by block_read()/block_write(). Returns
 * how many it registered. */
int nvme_init(void);

#endif
