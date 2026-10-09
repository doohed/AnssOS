#ifndef ARCH_X86_64_LAPIC_H
#define ARCH_X86_64_LAPIC_H

#include <stdint.h>

/* Local APIC register offsets (xAPIC MMIO layout; x2APIC reaches the same
 * registers as MSR 0x800 + offset/16, which lapic_read()/lapic_write()
 * handle). */
#define LAPIC_REG_TPR 0x080
#define LAPIC_REG_EOI 0x0B0
#define LAPIC_REG_SVR 0x0F0
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_LVT_LINT0 0x350
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CURRENT 0x390
#define LAPIC_REG_TIMER_DIVIDE 0x3E0

/* Software-enables this CPU's Local APIC and lets every interrupt
 * priority through (TPR = 0), in whichever mode the firmware left it:
 * xAPIC (registers in an MMIO page) or x2APIC (registers as MSRs, where
 * the MMIO page silently stops working -- a real PC's firmware may well
 * have switched to it, QEMU's never does). Idempotent. Must run after
 * pmm_init() (the xAPIC page is mapped through vmm_map_mmio()). */
void lapic_init(void);

int lapic_is_x2apic(void);
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);
void lapic_eoi(void);

#endif
