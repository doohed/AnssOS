//! Per-process x87/SSE register state, in the 512-byte FXSAVE/FXRSTOR
//! layout. The kernel itself never touches these registers (it's built
//! soft-float, see .cargo/config.toml) -- they
//! belong entirely to whichever ring-3 process is running, which is what
//! makes a plain eager save/restore at every dispatch (usermode.rs's
//! dispatch()) sufficient, with no lazy CR0.TS/#NM trapping needed.
//! FXSAVE/FXRSTOR need 16-byte alignment, which exec's FpuState type
//! guarantees.

use core::arch::asm;

pub const STATE_SIZE: usize = 512;

const CR0_MP: u64 = 1 << 1;
const CR0_EM: u64 = 1 << 2;
const CR0_TS: u64 = 1 << 3;
const CR0_NE: u64 = 1 << 5;
const CR4_OSFXSR: u64 = 1 << 9;
const CR4_OSXMMEXCPT: u64 = 1 << 10;

/// All x87 exceptions masked, 64-bit precision, round-to-nearest.
const FCW_DEFAULT: u16 = 0x037F;
/// All SIMD exceptions masked, round-to-nearest.
const MXCSR_DEFAULT: u32 = 0x1F80;

/// Enables x87/SSE for ring 3: clears CR0.EM/TS, sets CR0.MP/NE and
/// CR4.OSFXSR/OSXMMEXCPT. Before this, the first SSE instruction any
/// userland program executes raises #UD (or #NM).
pub fn init() {
    unsafe {
        let mut cr0: u64;
        asm!("mov {}, cr0", out(reg) cr0, options(nomem, nostack, preserves_flags));
        cr0 = cr0 & !(CR0_EM | CR0_TS) | CR0_MP | CR0_NE;
        asm!("mov cr0, {}", in(reg) cr0, options(nostack, preserves_flags));

        let mut cr4: u64;
        asm!("mov {}, cr4", out(reg) cr4, options(nomem, nostack, preserves_flags));
        cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
        asm!("mov cr4, {}", in(reg) cr4, options(nostack, preserves_flags));

        asm!("fninit", options(nomem, nostack));
    }
}

/// Fills `area` with the clean state a fresh process starts with (what
/// FNINIT plus the reset MXCSR leave): every x87/SIMD exception masked,
/// round-to-nearest, empty x87 stack, zeroed XMM registers. A zero-filled
/// area is *not* that -- FCW=0/MXCSR=0 unmask every floating-point
/// exception.
pub fn state_init(area: &mut [u8; STATE_SIZE]) {
    area.fill(0);
    // FXSAVE layout: FCW at offset 0, abridged FTW at 4 (0 = every
    // register empty), MXCSR at 24.
    area[0..2].copy_from_slice(&FCW_DEFAULT.to_le_bytes());
    area[24..28].copy_from_slice(&MXCSR_DEFAULT.to_le_bytes());
}

pub fn save(area: *mut u8) {
    unsafe { asm!("fxsave64 [{}]", in(reg) area, options(nostack, preserves_flags)) };
}

pub fn restore(area: *const u8) {
    unsafe { asm!("fxrstor64 [{}]", in(reg) area, options(readonly, nostack, preserves_flags)) };
}
