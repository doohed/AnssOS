#ifndef DRIVERS_PARTITION_H
#define DRIVERS_PARTITION_H

#include "block.h"

/* Reads a disk's partition table (GPT, checking both of its CRCs, or an
 * MBR) and prints each partition with its type, size, name and the
 * filesystem found in it -- read-only, for `lsblk` and the boot log. */
void partition_report(struct block_device *dev);

#endif
