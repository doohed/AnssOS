#include "lapic.h"
#include "../../mm/vmm.h"

#include <stddef.h>
#include <stdint.h>

#define IA32_APIC_BASE_MSR 0x1B
#define APIC_BASE_X2APIC_ENABLE (1u << 10)
#define APIC_BASE_ADDR_MASK 0xFFFFF000ull
#define X2APIC_MSR_BASE 0x800

static int initialized;
static int x2apic;
static volatile uint32_t *mmio;

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value) {
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

void lapic_init(void) {
    if (initialized) {
        return;
    }
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    x2apic = (base & APIC_BASE_X2APIC_ENABLE) != 0;
    if (!x2apic) {
        mmio = (volatile uint32_t *)vmm_map_mmio(base & APIC_BASE_ADDR_MASK, 0x1000);
    }
    initialized = 1;

    /* Spurious Interrupt Vector Register, bit 8: software-enables the
     * LAPIC -- without it every LVT write is inert even though the MSR's
     * global enable bit is already set. Spurious vector 0xFF is
     * conventional (unused; harmless if it ever actually fires). */
    lapic_write(LAPIC_REG_SVR, 0x1FF);
    /* Firmware may leave a nonzero task priority behind, which would
     * silently block every vector at or below it -- ours included. */
    lapic_write(LAPIC_REG_TPR, 0);
}

int lapic_is_x2apic(void) {
    return x2apic;
}

uint32_t lapic_read(uint32_t reg) {
    if (x2apic) {
        return (uint32_t)rdmsr(X2APIC_MSR_BASE + (reg >> 4));
    }
    return mmio[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t value) {
    if (x2apic) {
        wrmsr(X2APIC_MSR_BASE + (reg >> 4), value);
        return;
    }
    mmio[reg / 4] = value;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}
