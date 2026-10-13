//! Flat GDT: ring 0 code/data, ring 3 code/data (M10, for userspace), and
//! a TSS -- needed in long mode, and what supplies the kernel stack the
//! CPU switches to on an interrupt from ring 3. Selectors are fixed by the
//! layout of `GdtTable` below. The ring-3 selectors have RPL=3 baked in,
//! since they're only ever loaded while actually running at CPL 3.

use core::arch::asm;

use crate::sync::Global;

pub const SEL_KCODE: u16 = 0x08;
pub const SEL_KDATA: u16 = 0x10;
pub const SEL_UCODE: u16 = 0x18 | 3;
pub const SEL_UDATA: u16 = 0x20 | 3;
pub const SEL_TSS: u16 = 0x28;

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct GdtEntry {
    limit_low: u16,
    base_low: u16,
    base_mid: u8,
    access: u8,
    granularity: u8,
    base_high: u8,
}

impl GdtEntry {
    const NULL: GdtEntry = GdtEntry { limit_low: 0, base_low: 0, base_mid: 0, access: 0, granularity: 0, base_high: 0 };

    /// A flat segment: base 0, limit 4 GiB. `flags` is the high nibble of
    /// the granularity byte (G, D/B, L).
    const fn flat(access: u8, flags: u8) -> GdtEntry {
        GdtEntry { limit_low: 0xFFFF, base_low: 0, base_mid: 0, access, granularity: 0x0F | flags, base_high: 0 }
    }
}

/// A TSS descriptor is a 16-byte "system" descriptor -- twice the size of
/// a normal code/data one -- because it needs a full 64-bit base.
#[repr(C, packed)]
struct TssDescriptor {
    low: GdtEntry,
    base_upper: u32,
    reserved: u32,
}

#[repr(C, packed)]
struct GdtTable {
    null: GdtEntry,
    kcode: GdtEntry,
    kdata: GdtEntry,
    ucode: GdtEntry,
    udata: GdtEntry,
    tss: TssDescriptor,
}

#[repr(C, packed)]
struct Tss {
    reserved0: u32,
    rsp0: u64,
    rsp1: u64,
    rsp2: u64,
    reserved1: u64,
    ist: [u64; 7],
    reserved2: u64,
    reserved3: u16,
    iopb_offset: u16,
}

#[repr(C, packed)]
struct Gdtr {
    limit: u16,
    base: u64,
}

static TSS: Global<Tss> = Global::new(Tss {
    reserved0: 0,
    rsp0: 0,
    rsp1: 0,
    rsp2: 0,
    reserved1: 0,
    ist: [0; 7],
    reserved2: 0,
    reserved3: 0,
    iopb_offset: size_of::<Tss>() as u16, // No I/O permission bitmap.
});

static GDT: Global<GdtTable> = Global::new(GdtTable {
    null: GdtEntry::NULL,
    kcode: GdtEntry::flat(0x9A, 0xA0), // P, DPL0, code; L=1
    kdata: GdtEntry::flat(0x92, 0xC0), // P, DPL0, data
    ucode: GdtEntry::flat(0xFA, 0xA0), // P, DPL3, code; L=1
    udata: GdtEntry::flat(0xF2, 0xC0), // P, DPL3, data
    tss: TssDescriptor { low: GdtEntry::NULL, base_upper: 0, reserved: 0 }, // Filled in by gdt_init().
});

pub fn init() {
    let gdt = unsafe { GDT.get() };
    let base = TSS.as_ptr() as u64;
    let limit = size_of::<Tss>() as u32 - 1;
    gdt.tss = TssDescriptor {
        low: GdtEntry {
            limit_low: limit as u16,
            base_low: base as u16,
            base_mid: (base >> 16) as u8,
            access: 0x89, // present, DPL0, type 0x9 = 64-bit TSS (available)
            granularity: (limit >> 16) as u8 & 0x0F,
            base_high: (base >> 24) as u8,
        },
        base_upper: (base >> 32) as u32,
        reserved: 0,
    };

    let gdtr = Gdtr { limit: size_of::<GdtTable>() as u16 - 1, base: gdt as *mut GdtTable as u64 };
    unsafe {
        asm!("lgdt [{}]", in(reg) &gdtr, options(readonly, nostack, preserves_flags));
        // Reload the data segments, then CS via a far return -- in long
        // mode CS can't simply be moved into.
        asm!(
            "mov ds, {data:x}",
            "mov es, {data:x}",
            "mov fs, {data:x}",
            "mov gs, {data:x}",
            "mov ss, {data:x}",
            "push {code}",
            "lea {tmp}, [rip + 2f]",
            "push {tmp}",
            "retfq",
            "2:",
            data = in(reg) SEL_KDATA as u64,
            code = const SEL_KCODE as u64,
            tmp = out(reg) _,
        );
        asm!("ltr {:x}", in(reg) SEL_TSS, options(nostack, preserves_flags));
    }
}

/// Sets the kernel stack pointer (TSS.RSP0) the CPU switches to on any
/// interrupt, exception or syscall that arrives from ring 3 -- must be
/// valid before ever entering ring 3, and is updated per task.
pub fn set_kernel_stack(rsp0: u64) {
    unsafe { TSS.get().rsp0 = rsp0 };
}
