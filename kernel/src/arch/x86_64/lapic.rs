//! The Local APIC, in whichever mode the firmware left it: xAPIC
//! (registers in an MMIO page) or x2APIC (registers as MSRs 0x800 +
//! offset/16, where the MMIO page silently stops working -- a real PC's
//! firmware may well have switched to it, QEMU's never does).

use super::{rdmsr, wrmsr};
use crate::mm::vmm;
use crate::sync::Global;

pub const REG_TPR: u32 = 0x080;
pub const REG_EOI: u32 = 0x0B0;
pub const REG_SVR: u32 = 0x0F0;
pub const REG_LVT_TIMER: u32 = 0x320;
pub const REG_LVT_LINT0: u32 = 0x350;
pub const REG_TIMER_INIT: u32 = 0x380;
pub const REG_TIMER_CURRENT: u32 = 0x390;
pub const REG_TIMER_DIVIDE: u32 = 0x3E0;

const IA32_APIC_BASE_MSR: u32 = 0x1B;
const APIC_BASE_X2APIC_ENABLE: u64 = 1 << 10;
const APIC_BASE_ADDR_MASK: u64 = 0xFFFF_F000;
const X2APIC_MSR_BASE: u32 = 0x800;

struct Lapic {
    initialized: bool,
    x2apic: bool,
    mmio: *mut u32,
}

static LAPIC: Global<Lapic> = Global::new(Lapic { initialized: false, x2apic: false, mmio: core::ptr::null_mut() });

fn lapic() -> &'static mut Lapic {
    unsafe { LAPIC.get() }
}

/// Software-enables this CPU's Local APIC and lets every interrupt
/// priority through (TPR = 0). Idempotent. Must run after the PMM is up
/// (the xAPIC page is mapped through vmm::map_mmio()).
pub fn init() {
    let l = lapic();
    if l.initialized {
        return;
    }
    let base = rdmsr(IA32_APIC_BASE_MSR);
    l.x2apic = base & APIC_BASE_X2APIC_ENABLE != 0;
    if !l.x2apic {
        l.mmio = vmm::map_mmio(base & APIC_BASE_ADDR_MASK, 0x1000).cast();
    }
    l.initialized = true;

    // Spurious Interrupt Vector Register, bit 8: software-enables the
    // LAPIC -- without it every LVT write is inert even though the MSR's
    // global enable bit is already set. Spurious vector 0xFF is
    // conventional (unused; harmless if it ever actually fires).
    write(REG_SVR, 0x1FF);
    // Firmware may leave a nonzero task priority behind, which would
    // silently block every vector at or below it -- ours included.
    write(REG_TPR, 0);
}

pub fn is_x2apic() -> bool {
    lapic().x2apic
}

pub fn read(reg: u32) -> u32 {
    let l = lapic();
    if l.x2apic {
        return rdmsr(X2APIC_MSR_BASE + (reg >> 4)) as u32;
    }
    unsafe { l.mmio.add(reg as usize / 4).read_volatile() }
}

pub fn write(reg: u32, value: u32) {
    let l = lapic();
    if l.x2apic {
        wrmsr(X2APIC_MSR_BASE + (reg >> 4), value as u64);
        return;
    }
    unsafe { l.mmio.add(reg as usize / 4).write_volatile(value) };
}

pub fn eoi() {
    write(REG_EOI, 0);
}
