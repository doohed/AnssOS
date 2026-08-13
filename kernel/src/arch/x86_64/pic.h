#ifndef ARCH_X86_64_PIC_H
#define ARCH_X86_64_PIC_H

#include <stdint.h>

/* IRQ0-15 land on interrupt vectors PIC_IRQ_BASE..PIC_IRQ_BASE+15 once
 * remapped -- clear of the 0-31 CPU exception range (the classic "IRQ7
 * looks like a #GP" bug if you forget to do this before enabling
 * interrupts). */
#define PIC_IRQ_BASE 32

/* Remaps the master/slave 8259 pair via the standard ICW1-ICW4 sequence,
 * masks every line (nothing fires until a driver explicitly unmasks its
 * own IRQ with pic_clear_mask()), and configures the Local APIC to
 * actually relay the legacy PIC's INTR line to the CPU (LINT0 -> ExtINT,
 * unmasked -- required on chipsets that default to APIC-only routing,
 * QEMU's q35 included; see the comment on lapic_enable_extint_passthrough()
 * in pic.c for how this was diagnosed). Must run after pmm_init()
 * (it maps the LAPIC's MMIO page, which allocates page tables through
 * the PMM) and before `sti`. */
void pic_remap(void);

void pic_send_eoi(uint8_t irq);
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

#endif
