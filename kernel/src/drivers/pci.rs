//! PCI enumeration over the legacy 0xCF8/0xCFC configuration ports: a
//! brute-force scan of all 256 buses x 32 slots x (1 or 8) functions.
//! Fine for QEMU's flat topology; a real machine would want to walk the
//! ACPI MCFG table for ECAM instead (out of scope for now -- see the
//! roadmap).

use crate::arch::x86_64::io::{inl, outb, outl, outw};
use crate::sync::Global;

pub const MAX_DEVICES: usize = 64;

const CONFIG_ADDRESS: u16 = 0xCF8;
const CONFIG_DATA: u16 = 0xCFC;

#[derive(Clone, Copy)]
pub struct PciDevice {
    pub bus: u8,
    pub slot: u8,
    pub func: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub revision: u8,
    pub prog_if: u8,
    pub subclass: u8,
    pub class_code: u8,
    pub header_type: u8,
    /// Only valid for header_type 0x00 (normal devices).
    pub bar: [u32; 6],
}

impl PciDevice {
    const EMPTY: PciDevice = PciDevice {
        bus: 0,
        slot: 0,
        func: 0,
        vendor_id: 0,
        device_id: 0,
        revision: 0,
        prog_if: 0,
        subclass: 0,
        class_code: 0,
        header_type: 0,
        bar: [0; 6],
    };

    pub fn read8(&self, offset: u8) -> u8 {
        read8(self.bus, self.slot, self.func, offset)
    }
    pub fn read16(&self, offset: u8) -> u16 {
        read16(self.bus, self.slot, self.func, offset)
    }
    pub fn read32(&self, offset: u8) -> u32 {
        read32(self.bus, self.slot, self.func, offset)
    }
    pub fn write8(&self, offset: u8, value: u8) {
        write8(self.bus, self.slot, self.func, offset, value)
    }
    pub fn write16(&self, offset: u8, value: u16) {
        write16(self.bus, self.slot, self.func, offset, value)
    }
    pub fn write32(&self, offset: u8, value: u32) {
        write32(self.bus, self.slot, self.func, offset, value)
    }
}

struct Pci {
    devices: [PciDevice; MAX_DEVICES],
    count: usize,
}

static PCI: Global<Pci> = Global::new(Pci { devices: [PciDevice::EMPTY; MAX_DEVICES], count: 0 });

fn pci() -> &'static mut Pci {
    unsafe { PCI.get() }
}

fn address(bus: u8, slot: u8, func: u8, offset: u8) -> u32 {
    1 << 31 | (bus as u32) << 16 | ((slot & 0x1F) as u32) << 11 | ((func & 0x07) as u32) << 8 | (offset & 0xFC) as u32
}

pub fn read32(bus: u8, slot: u8, func: u8, offset: u8) -> u32 {
    outl(CONFIG_ADDRESS, address(bus, slot, func, offset));
    inl(CONFIG_DATA)
}

pub fn read16(bus: u8, slot: u8, func: u8, offset: u8) -> u16 {
    (read32(bus, slot, func, offset) >> ((offset & 2) * 8)) as u16
}

pub fn read8(bus: u8, slot: u8, func: u8, offset: u8) -> u8 {
    (read32(bus, slot, func, offset) >> ((offset & 3) * 8)) as u8
}

pub fn write32(bus: u8, slot: u8, func: u8, offset: u8, value: u32) {
    outl(CONFIG_ADDRESS, address(bus, slot, func, offset));
    outl(CONFIG_DATA, value);
}

pub fn write16(bus: u8, slot: u8, func: u8, offset: u8, value: u16) {
    outl(CONFIG_ADDRESS, address(bus, slot, func, offset));
    outw(CONFIG_DATA + (offset & 2) as u16, value);
}

pub fn write8(bus: u8, slot: u8, func: u8, offset: u8, value: u8) {
    outl(CONFIG_ADDRESS, address(bus, slot, func, offset));
    outb(CONFIG_DATA + (offset & 3) as u16, value);
}

fn class_name(class_code: u8) -> &'static str {
    match class_code {
        0x00 => "Unclassified",
        0x01 => "Mass Storage",
        0x02 => "Network",
        0x03 => "Display",
        0x04 => "Multimedia",
        0x05 => "Memory",
        0x06 => "Bridge",
        0x0C => "Serial Bus",
        _ => "Other",
    }
}

fn print_device(d: &PciDevice) {
    kprintln!(
        "PCI {}:{}.{}  {:x}:{:x}  class {:x}.{:x} ({})",
        d.bus,
        d.slot,
        d.func,
        d.vendor_id,
        d.device_id,
        d.class_code,
        d.subclass,
        class_name(d.class_code)
    );
}

fn probe_function(bus: u8, slot: u8, func: u8) {
    let p = pci();
    let vendor_id = read16(bus, slot, func, 0x00);
    if vendor_id == 0xFFFF || p.count >= MAX_DEVICES {
        return;
    }
    let mut d = PciDevice {
        bus,
        slot,
        func,
        vendor_id,
        device_id: read16(bus, slot, func, 0x02),
        revision: read8(bus, slot, func, 0x08),
        prog_if: read8(bus, slot, func, 0x09),
        subclass: read8(bus, slot, func, 0x0A),
        class_code: read8(bus, slot, func, 0x0B),
        header_type: read8(bus, slot, func, 0x0E),
        bar: [0; 6],
    };
    if d.header_type & 0x7F == 0x00 {
        for (i, bar) in d.bar.iter_mut().enumerate() {
            *bar = read32(bus, slot, func, 0x10 + i as u8 * 4);
        }
    }
    print_device(&d);
    p.devices[p.count] = d;
    p.count += 1;
}

pub fn enumerate() {
    pci().count = 0;
    for bus in 0..=255u8 {
        for slot in 0..32 {
            if read16(bus, slot, 0, 0x00) == 0xFFFF {
                continue;
            }
            let max_func = if read8(bus, slot, 0, 0x0E) & 0x80 != 0 { 8 } else { 1 };
            for func in 0..max_func {
                probe_function(bus, slot, func);
            }
        }
    }
    kprintln!("PCI: {} device(s) found", pci().count);
}

pub fn devices() -> &'static [PciDevice] {
    let p = pci();
    &p.devices[..p.count]
}

/// The `index`'th (0-based) device with this vendor:device id -- needed
/// for e.g. virtio-input, where QEMU exposes keyboard, mouse and tablet
/// under the identical id and the caller inspects each one to tell them
/// apart.
pub fn find_device_nth(vendor_id: u16, device_id: u16, index: usize) -> Option<&'static PciDevice> {
    devices().iter().filter(|d| d.vendor_id == vendor_id && d.device_id == device_id).nth(index)
}

pub fn find_device(vendor_id: u16, device_id: u16) -> Option<&'static PciDevice> {
    find_device_nth(vendor_id, device_id, 0)
}

/// The `index`'th device of a class/subclass/programming interface -- how
/// drivers for standard controllers find theirs (xHCI is 0C.03 prog-if
/// 30), whatever the vendor.
pub fn find_class_nth(class_code: u8, subclass: u8, prog_if: u8, index: usize) -> Option<&'static PciDevice> {
    devices().iter().filter(|d| d.class_code == class_code && d.subclass == subclass && d.prog_if == prog_if).nth(index)
}

/// Re-lists what pci_enumerate() found, without rescanning -- the shell's
/// `lspci`.
pub fn print_devices() {
    devices().iter().for_each(print_device);
    kprintln!("PCI: {} device(s)", devices().len());
}
