#ifndef DRIVERS_DISKTEST_H
#define DRIVERS_DISKTEST_H

#include "block.h"

/* Storage driver checks for the kernel shell's `disktest`.
 *
 * Read check (always safe): reads the first MiB twice and compares, then
 * times a sequential read of up to 64 MiB.
 *
 * Write check (`write` nonzero; only with allow-disk-writes): in a 1 MiB
 * region in the middle of the disk, saves what's there, writes a
 * pattern, reads it back and compares, then writes the original back and
 * verifies that too -- so even the write check leaves the disk as it was.
 * Returns 0 if everything matched. */
int disktest_run(struct block_device *dev, int write);

#endif
