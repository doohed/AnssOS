#ifndef DRIVERS_TIMER_H
#define DRIVERS_TIMER_H

#include <stdint.h>

#define TIMER_HZ 100 /* 100 ticks/sec = 10 ms/tick. */

/* The kernel's tick: TIMER_HZ interrupts a second on vector PIC_IRQ_BASE
 * (dispatched as "IRQ0" by arch/x86_64/idt.c, which is also what drives
 * preemption). Sources, best first:
 *
 *   1. The Local APIC timer, calibrated against the ACPI PM timer (or
 *      PIT channel 2, polled, if there's no PM timer). It's inside the
 *      CPU, so no interrupt routing is involved -- what makes it work on
 *      a real PC whose board doesn't wire the old 8259 to the CPU.
 *   2. PIT channel 0 through the 8259, as on QEMU.
 *   3. None: timer_check() found no ticks arriving at all. Sleeps become
 *      busy-waits and there's no preemption, but boot carries on.
 *
 * timer_init() picks 1 or 2 (after pic_remap(), before `sti`);
 * timer_check() (right after `sti`) confirms ticks really arrive and
 * steps down the list if not. */
void timer_init(void);
void timer_check(void);

/* For idt.c: whether the tick needs a Local APIC EOI instead of an 8259
 * one. */
int timer_uses_lapic(void);

uint64_t timer_ticks(void);
uint64_t timer_uptime_ms(void);

/* Waits at least `ms` milliseconds (sti; hlt between ticks, so it can't
 * yield to anything else). */
void timer_sleep_ms(uint32_t ms);

/* One moment of a busy-wait: lets a pending tick in, then puts the
 * interrupt flag back the way it was. Inside a syscall (an interrupt
 * gate: interrupts off) a long wait would otherwise stop the clock --
 * uptime freezes and timeouts measured with timer_uptime_ms() never
 * expire. */
void timer_idle(void);

/* One line describing the source in use, via kprintf -- for the boot log,
 * where it's the first thing to check if a real PC hangs at boot. */
void timer_log_status(void);

#endif
