//! x86_64 specifics: descriptor tables, interrupt controllers, the FPU,
//! entering ring 3, port I/O, and the few instructions the rest of the
//! kernel needs directly.

pub mod fpu;
pub mod gdt;
pub mod idt;
pub mod io;
pub mod lapic;
pub mod pic;
mod stubs;
pub mod usermode;

use core::arch::asm;

/// Stops this CPU for good: interrupts off, then `hlt` forever. What a
/// fatal error at boot ends in.
pub fn halt_forever() -> ! {
    loop {
        unsafe { asm!("cli; hlt", options(nomem, nostack)) };
    }
}

pub fn rdmsr(msr: u32) -> u64 {
    let (lo, hi): (u32, u32);
    unsafe { asm!("rdmsr", in("ecx") msr, out("eax") lo, out("edx") hi, options(nomem, nostack, preserves_flags)) };
    (hi as u64) << 32 | lo as u64
}

pub fn wrmsr(msr: u32, value: u64) {
    unsafe {
        asm!("wrmsr", in("ecx") msr, in("eax") value as u32, in("edx") (value >> 32) as u32, options(nostack, preserves_flags))
    };
}

pub fn rdtsc() -> u64 {
    let (lo, hi): (u32, u32);
    unsafe { asm!("rdtsc", out("eax") lo, out("edx") hi, options(nomem, nostack, preserves_flags)) };
    (hi as u64) << 32 | lo as u64
}

/// CPUID leaf `leaf`, subleaf 0: (eax, ebx, ecx, edx).
pub fn cpuid(leaf: u32) -> (u32, u32, u32, u32) {
    let r = core::arch::x86_64::__cpuid_count(leaf, 0);
    (r.eax, r.ebx, r.ecx, r.edx)
}

/// Writes back and evicts the cache lines covering `len` bytes at `p`,
/// then fences: for DMA with a device that may not snoop CPU caches
/// (AMD's HD Audio, unless told to) -- what it reads must be in RAM, and
/// what it wrote mustn't be shadowed by stale lines.
pub fn clflush_range(p: *const u8, len: usize) {
    let mut a = p as usize & !63;
    while a < p as usize + len {
        unsafe { asm!("clflush [{}]", in(reg) a, options(nostack, preserves_flags)) };
        a += 64;
    }
    unsafe { asm!("mfence", options(nostack, preserves_flags)) };
}

pub fn read_cr2() -> u64 {
    let cr2: u64;
    unsafe { asm!("mov {}, cr2", out(reg) cr2, options(nomem, nostack, preserves_flags)) };
    cr2
}

pub fn enable_interrupts() {
    unsafe { asm!("sti", options(nomem, nostack)) };
}

pub fn interrupts_enabled() -> bool {
    let flags: u64;
    unsafe { asm!("pushfq; pop {}", out(reg) flags, options(nomem, preserves_flags)) };
    flags & 0x200 != 0
}

/// One idle moment with interrupts let in, then off again -- for a wait
/// inside a syscall (an interrupt gate, so interrupts are off): without
/// it the tick stops for as long as the wait lasts.
pub fn idle_with_interrupts() {
    unsafe { asm!("sti; pause; cli", options(nomem, nostack)) };
}

/// Waits for the next interrupt with interrupts enabled.
pub fn wait_for_interrupt() {
    unsafe { asm!("sti; hlt", options(nomem, nostack)) };
}
