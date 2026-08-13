#include "pit.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/io.h"
#include "../arch/x86_64/pic.h"

#include <stdint.h>

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND 0x43
#define PIT_BASE_FREQ 1193182

static volatile uint64_t ticks;

static void pit_irq_handler(void) {
    ticks++;
}

void pit_init(void) {
    uint16_t divisor = (uint16_t)(PIT_BASE_FREQ / PIT_HZ);

    outb(PIT_COMMAND, 0x36); /* Channel 0, lobyte/hibyte access, mode 3 (square wave), binary. */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));

    irq_register(0, pit_irq_handler);
    pic_clear_mask(0);
}

uint64_t pit_ticks(void) {
    return ticks;
}

uint64_t pit_uptime_ms(void) {
    return ticks * (1000 / PIT_HZ);
}

void pit_sleep_ms(uint32_t ms) {
    if (ms == 0) {
        return;
    }

    uint32_t ms_per_tick = 1000 / PIT_HZ;
    uint64_t ticks_needed = (ms + ms_per_tick - 1) / ms_per_tick;
    if (ticks_needed == 0) {
        ticks_needed = 1;
    }

    uint64_t target = ticks + ticks_needed;
    while (ticks < target) {
        asm volatile("sti; hlt");
    }
}
