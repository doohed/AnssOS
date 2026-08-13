#include "pic.h"
#include "io.h"
#include "../../mm/vmm.h"

#include <stdint.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01
#define PIC_EOI 0x20

#define IA32_APIC_BASE_MSR 0x1B
#define APIC_BASE_ADDR_MASK 0xFFFFF000ull
#define APIC_REG_SVR 0x0F0
#define APIC_REG_LVT_LINT0 0x350

/* On chipsets that default to APIC-only interrupt routing (QEMU's q35
 * included), a fully-correct PIC+PIT setup can sit with IRQs raised in
 * the 8259's IRR forever and never reach the CPU: real hardware wires
 * the master 8259's INTR line to the BSP's Local APIC LINT0 pin, and
 * that only relays it if LINT0 is configured for ExtINT delivery and
 * unmasked, and the LAPIC itself is software-enabled -- neither of which
 * is guaranteed by firmware once it's done with its own boot phase.
 * Confirmed via QEMU's `info pic` (irr showed IRQ0 pending, isr stayed
 * 0) that this step, not the PIC/PIT programming, was the missing
 * piece. Must run after the PMM is up (vmm_map_mmio() allocates page
 * tables through it). */
static void lapic_enable_extint_passthrough(void) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(IA32_APIC_BASE_MSR));
    uint64_t apic_base_phys = (((uint64_t)hi << 32) | lo) & APIC_BASE_ADDR_MASK;

    volatile uint32_t *apic = (volatile uint32_t *)vmm_map_mmio(apic_base_phys, 0x1000);

    /* Spurious Interrupt Vector Register, bit 8: software-enables the
     * LAPIC -- without this the LVT write below is inert even though the
     * MSR's global enable bit is already set. Spurious vector 0xFF is
     * conventional (unused; harmless if it ever actually fires). */
    apic[APIC_REG_SVR / 4] = 0x1FF;

    /* LVT LINT0 = ExtINT (delivery mode 111, bits 10:8) and unmasked
     * (bit 16 clear). */
    apic[APIC_REG_LVT_LINT0 / 4] = 0x700;
}

void pic_remap(void) {
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, PIC_IRQ_BASE); /* Master: IRQ0-7 -> vectors 32-39. */
    io_wait();
    outb(PIC2_DATA, PIC_IRQ_BASE + 8); /* Slave: IRQ8-15 -> vectors 40-47. */
    io_wait();

    outb(PIC1_DATA, 0x04); /* Tell master there's a slave wired to IRQ2. */
    io_wait();
    outb(PIC2_DATA, 0x02); /* Tell slave its cascade identity. */
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Mask everything -- drivers unmask their own line when they're ready. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    /* IMCR: on chipsets that implement it (present on Q35/ICH9), the
     * legacy 8259 pair's output isn't necessarily wired to the CPU at
     * all until this says so -- select IMCR (0x70 to the select port),
     * then select PIC mode (0x01) over "virtual wire via APIC" mode.
     * Harmless on chipsets without an IMCR: an unimplemented I/O port
     * write is simply ignored. */
    outb(0x22, 0x70);
    outb(0x23, 0x01);

    lapic_enable_extint_passthrough();
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t line = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(port, (uint8_t)(inb(port) | (1u << line)));
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t line = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(port, (uint8_t)(inb(port) & ~(1u << line)));
}
