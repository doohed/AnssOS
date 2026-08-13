#ifndef DRIVERS_PIT_H
#define DRIVERS_PIT_H

#include <stdint.h>

#define PIT_HZ 100 /* 100 ticks/sec = 10 ms/tick. */

/* Programs PIT channel 0 (mode 3, square wave) for PIT_HZ, registers its
 * IRQ0 handler (see arch/x86_64/idt.h's irq_register()), and unmasks
 * IRQ0 on the PIC. arch/x86_64/idt_init() and pic_remap() must have
 * already run; nothing ticks until interrupts are globally enabled
 * (`sti`) afterward. */
void pit_init(void);

uint64_t pit_ticks(void);
uint64_t pit_uptime_ms(void);

/* Waits (sti; hlt per iteration -- woken by any interrupt, not just the
 * timer, but re-checks and goes back to sleep if it wasn't enough) until
 * at least `ms` milliseconds have passed. There's no scheduler yet, so
 * this can't yield to anything else; it's still strictly better than a
 * guessed busy-wait spin count. */
void pit_sleep_ms(uint32_t ms);

#endif
