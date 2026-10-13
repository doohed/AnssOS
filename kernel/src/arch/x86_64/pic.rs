//! The legacy 8259 PIC pair, remapped so IRQ0-15 land on vectors
//! PIC_IRQ_BASE..PIC_IRQ_BASE+15 -- clear of the 0-31 CPU exception range
//! (the classic "IRQ7 looks like a #GP" bug if this is skipped before
//! enabling interrupts).

use super::io::{inb, io_wait, outb};
use super::lapic;

pub const PIC_IRQ_BASE: u8 = 32;

const PIC1_COMMAND: u16 = 0x20;
const PIC1_DATA: u16 = 0x21;
const PIC2_COMMAND: u16 = 0xA0;
const PIC2_DATA: u16 = 0xA1;
const ICW1_INIT: u8 = 0x10;
const ICW1_ICW4: u8 = 0x01;
const ICW4_8086: u8 = 0x01;
const PIC_EOI: u8 = 0x20;

/// On chipsets that default to APIC-only interrupt routing (QEMU's q35
/// included), a fully-correct PIC+PIT setup can sit with IRQs raised in
/// the 8259's IRR forever and never reach the CPU: real hardware wires the
/// master 8259's INTR line to the BSP's Local APIC LINT0 pin, and that
/// only relays it if LINT0 is configured for ExtINT delivery and
/// unmasked, and the LAPIC itself is software-enabled -- neither of which
/// is guaranteed by firmware once it's done with its own boot phase.
/// Confirmed via QEMU's `info pic` (irr showed IRQ0 pending, isr stayed 0)
/// that this step, not the PIC/PIT programming, was the missing piece.
fn lapic_enable_extint_passthrough() {
    lapic::init(); // Software-enables the LAPIC -- the LVT write below is inert without it.
    // LVT LINT0 = ExtINT (delivery mode 111, bits 10:8), unmasked (bit 16 clear).
    lapic::write(lapic::REG_LVT_LINT0, 0x700);
}

/// Remaps the pair via the standard ICW1-ICW4 sequence, masks every line
/// (nothing fires until a driver unmasks its own with pic_clear_mask()),
/// and has the Local APIC relay the PIC's INTR line to the CPU (see
/// above). Must run after the PMM is up (the LAPIC's MMIO page is mapped
/// through it) and before `sti`.
pub fn remap() {
    let steps: [(u16, u8); 8] = [
        (PIC1_COMMAND, ICW1_INIT | ICW1_ICW4),
        (PIC2_COMMAND, ICW1_INIT | ICW1_ICW4),
        (PIC1_DATA, PIC_IRQ_BASE),     // Master: IRQ0-7 -> vectors 32-39.
        (PIC2_DATA, PIC_IRQ_BASE + 8), // Slave: IRQ8-15 -> vectors 40-47.
        (PIC1_DATA, 0x04),             // Tell master there's a slave wired to IRQ2.
        (PIC2_DATA, 0x02),             // Tell slave its cascade identity.
        (PIC1_DATA, ICW4_8086),
        (PIC2_DATA, ICW4_8086),
    ];
    for (port, value) in steps {
        outb(port, value);
        io_wait();
    }

    // Mask everything -- drivers unmask their own line when they're ready.
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    // IMCR: on chipsets that implement it (present on Q35/ICH9), the
    // legacy 8259 pair's output isn't necessarily wired to the CPU at all
    // until this says so -- select IMCR (0x70 to the select port), then
    // select PIC mode (0x01) over "virtual wire via APIC" mode. Harmless
    // on chipsets without an IMCR: an unimplemented port write is ignored.
    outb(0x22, 0x70);
    outb(0x23, 0x01);

    lapic_enable_extint_passthrough();
}

pub fn send_eoi(irq: u8) {
    if irq >= 8 {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

fn line(irq: u8) -> (u16, u8) {
    if irq < 8 { (PIC1_DATA, irq) } else { (PIC2_DATA, irq - 8) }
}

pub fn set_mask(irq: u8) {
    let (port, line) = line(irq);
    outb(port, inb(port) | 1 << line);
}

pub fn clear_mask(irq: u8) {
    let (port, line) = line(irq);
    outb(port, inb(port) & !(1 << line));
}
