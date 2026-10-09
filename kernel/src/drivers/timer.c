#include "timer.h"
#include "acpi.h"
#include "pit.h"
#include "serial.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/lapic.h"
#include "../arch/x86_64/pic.h"

#include <stdint.h>

#define CALIBRATE_MS 50
#define LAPIC_TIMER_PERIODIC (1u << 17)
#define LAPIC_LVT_MASKED (1u << 16)
#define LAPIC_DIVIDE_BY_16 0x3

static volatile uint64_t ticks;
static enum { SOURCE_NONE, SOURCE_LAPIC, SOURCE_PIT } source;
static uint32_t lapic_counts_per_tick;
static const char *calibrated_against;
/* Whether the LAPIC timer keeps running while the CPU sleeps in `hlt`
 * (CPUID "ARAT"). Without it, `hlt` could wait forever for a tick that
 * the sleeping CPU stopped counting towards. */
static int lapic_always_running;

static void tick(void) {
    ticks++;
}

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Busy-waits `ms` on whatever needs no interrupts: the ACPI PM timer, or
 * PIT channel 2. Returns -1 if neither works. */
static int poll_wait_ms(uint32_t ms) {
    if (acpi_pm_timer_present()) {
        uint32_t target = (uint32_t)((uint64_t)ACPI_PM_TIMER_HZ * ms / 1000);
        uint32_t start = acpi_pm_timer_read();
        /* Bounded like pit_poll_wait_ms(): a stuck counter gives up
         * after a few seconds' worth of port reads. */
        for (uint64_t i = 0; i < 5000000ull + (uint64_t)ms * 5000; i++) {
            if (acpi_pm_timer_delta(start, acpi_pm_timer_read()) >= target) {
                return 0;
            }
        }
        return -1;
    }
    for (; ms > CALIBRATE_MS; ms -= CALIBRATE_MS) {
        if (pit_poll_wait_ms(CALIBRATE_MS) != 0) {
            return -1;
        }
    }
    return pit_poll_wait_ms(ms);
}

/* Counts how far the LAPIC timer gets in CALIBRATE_MS, then runs it
 * periodically at TIMER_HZ. Returns -1 if it can't be measured. */
static int start_lapic_timer(void) {
    lapic_init();
    lapic_write(LAPIC_REG_TIMER_DIVIDE, LAPIC_DIVIDE_BY_16);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);
    int waited = poll_wait_ms(CALIBRATE_MS);
    uint32_t elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_REG_TIMER_CURRENT);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);
    if (waited != 0) {
        kprintf("timer: nothing to calibrate the LAPIC timer against\n");
        return -1;
    }

    uint64_t per_tick = (uint64_t)elapsed * 1000 / TIMER_HZ / CALIBRATE_MS;
    if (per_tick < 100 || per_tick > 0xFFFFFFFFu) {
        kprintf("timer: LAPIC timer counted %u in %u ms -- not usable\n", elapsed, CALIBRATE_MS);
        return -1;
    }
    lapic_counts_per_tick = (uint32_t)per_tick;
    calibrated_against = acpi_pm_timer_present() ? "the ACPI PM timer" : "PIT channel 2";

    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(6), "c"(0));
    lapic_always_running = (eax >> 2) & 1;

    lapic_write(LAPIC_REG_LVT_TIMER, PIC_IRQ_BASE | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_INIT, lapic_counts_per_tick);
    return 0;
}

static void start_pit(void) {
    pit_start_periodic(TIMER_HZ);
    pic_clear_mask(0);
}

void timer_init(void) {
    irq_register(0, tick);
    acpi_init();
    if (start_lapic_timer() == 0) {
        source = SOURCE_LAPIC; /* IRQ0 stays masked on the 8259: one tick source only. */
    } else {
        start_pit();
        source = SOURCE_PIT;
    }
}

/* Whether a few ticks arrive within about 200 ms. The deadline is
 * measured without interrupts -- by the PM timer if there is one,
 * otherwise by a TSC count that's 200 ms or more at any clock speed a
 * PC running this has (it only has to bound the wait). */
static int ticks_arriving(void) {
    uint64_t start_ticks = ticks;
    int have_pm = acpi_pm_timer_present();
    uint32_t pm_start = have_pm ? acpi_pm_timer_read() : 0;
    uint64_t tsc_start = rdtsc();
    for (;;) {
        if (ticks - start_ticks >= 3) {
            return 1;
        }
        if (have_pm ? acpi_pm_timer_delta(pm_start, acpi_pm_timer_read()) > ACPI_PM_TIMER_HZ / 5
                    : rdtsc() - tsc_start > 1000000000ull) {
            return 0;
        }
        asm volatile("pause");
    }
}

void timer_check(void) {
    if (ticks_arriving()) {
        return;
    }
    if (source == SOURCE_LAPIC) {
        kprintf("timer: LAPIC timer ticks aren't arriving -- trying the PIT\n");
        lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
        start_pit();
        source = SOURCE_PIT;
        if (ticks_arriving()) {
            return;
        }
    }
    kprintf("timer: PIT ticks aren't arriving -- no timer interrupts at all\n");
    pic_set_mask(0);
    source = SOURCE_NONE;
}

int timer_uses_lapic(void) {
    return source == SOURCE_LAPIC;
}

uint64_t timer_ticks(void) {
    return ticks;
}

uint64_t timer_uptime_ms(void) {
    return ticks * (1000 / TIMER_HZ);
}

void timer_sleep_ms(uint32_t ms) {
    if (ms == 0) {
        return;
    }
    if (source == SOURCE_NONE) {
        poll_wait_ms(ms);
        return;
    }

    uint32_t ms_per_tick = 1000 / TIMER_HZ;
    uint64_t target = ticks + (ms + ms_per_tick - 1) / ms_per_tick;
    while (ticks < target) {
        if (source == SOURCE_LAPIC && !lapic_always_running) {
            asm volatile("sti; pause");
        } else {
            asm volatile("sti; hlt");
        }
    }
}

void timer_idle(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags));
    asm volatile("sti; pause" ::: "memory");
    if (!(flags & 0x200)) {
        asm volatile("cli" ::: "memory");
    }
}

void timer_log_status(void) {
    switch (source) {
        case SOURCE_LAPIC:
            kprintf("Timer: LAPIC timer (%s), %u counts/tick vs %s, %s\n",
                    lapic_is_x2apic() ? "x2APIC" : "xAPIC", lapic_counts_per_tick,
                    calibrated_against, lapic_always_running ? "ARAT" : "no ARAT");
            break;
        case SOURCE_PIT:
            kprintf("Timer: PIT through the 8259\n");
            break;
        default:
            kprintf("Timer: NONE -- no timer interrupts; sleeps busy-wait, no preemption\n");
            break;
    }
}
