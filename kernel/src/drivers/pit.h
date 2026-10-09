#ifndef DRIVERS_PIT_H
#define DRIVERS_PIT_H

#include <stdint.h>

/* The legacy 8254 Programmable Interval Timer. drivers/timer.c is what
 * the rest of the kernel uses; this is just the two things it needs from
 * the chip. */

#define PIT_BASE_FREQ 1193182

/* Channel 0 as a square wave at `hz`: IRQ0 on the 8259, if the board
 * actually routes that to the CPU (QEMU does; real PCs may not). */
void pit_start_periodic(uint32_t hz);

/* Busy-waits `ms` (at most 50) on channel 2, polled through port 0x61 --
 * no interrupt involved. Returns -1 if the channel never finishes,
 * e.g. a chipset with the 8254 switched off. */
int pit_poll_wait_ms(uint32_t ms);

#endif
