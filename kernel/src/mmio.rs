//! Volatile access to one field of a device-shared struct -- registers in
//! an MMIO region, or rings and descriptors the device DMAs. A field read
//! or written through these is never cached, merged or reordered against
//! another by the compiler -- what `volatile` gives in C.

/// `vread!(ptr, field)`: reads `(*ptr).field`.
macro_rules! vread {
    ($p:expr, $($field:tt)+) => {{
        #[allow(unused_unsafe)] // When used inside another unsafe block.
        let value = unsafe { (&raw const (*$p).$($field)+).read_volatile() };
        value
    }};
}

/// `vwrite!(ptr, field, value)`: writes `(*ptr).field`.
macro_rules! vwrite {
    ($p:expr, $field:ident, $v:expr) => {{
        let value = $v;
        unsafe { (&raw mut (*$p).$field).write_volatile(value) }
    }};
    ($p:expr, $field:ident [$i:expr], $v:expr) => {{
        let value = $v;
        unsafe { (&raw mut (*$p).$field[$i]).write_volatile(value) }
    }};
}

/// A device's register block, as a base pointer into its MMIO mapping.
/// Every access is volatile; 64-bit registers are written as two halves,
/// low first, which every device here accepts.
#[derive(Clone, Copy)]
pub struct Regs(pub *mut u8);

impl Regs {
    /// The block `off` bytes in -- e.g. one port's registers.
    pub fn at(self, off: u32) -> Regs {
        Regs(unsafe { self.0.add(off as usize) })
    }

    pub fn rd8(self, off: u32) -> u8 {
        unsafe { self.0.add(off as usize).read_volatile() }
    }
    pub fn rd16(self, off: u32) -> u16 {
        unsafe { self.0.add(off as usize).cast::<u16>().read_volatile() }
    }
    pub fn rd32(self, off: u32) -> u32 {
        unsafe { self.0.add(off as usize).cast::<u32>().read_volatile() }
    }
    pub fn rd64(self, off: u32) -> u64 {
        self.rd32(off) as u64 | (self.rd32(off + 4) as u64) << 32
    }
    pub fn wr8(self, off: u32, v: u8) {
        unsafe { self.0.add(off as usize).write_volatile(v) }
    }
    pub fn wr16(self, off: u32, v: u16) {
        unsafe { self.0.add(off as usize).cast::<u16>().write_volatile(v) }
    }
    pub fn wr32(self, off: u32, v: u32) {
        unsafe { self.0.add(off as usize).cast::<u32>().write_volatile(v) }
    }
    pub fn wr64(self, off: u32, v: u64) {
        self.wr32(off, v as u32);
        self.wr32(off + 4, (v >> 32) as u32);
    }
}
