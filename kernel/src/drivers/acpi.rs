//! Just enough ACPI for the PM timer: a free-running counter at a fixed
//! 3.579545 MHz that every x86 PC with ACPI has, readable with a plain
//! `in` -- no interrupt routing, nothing to program. What drivers/timer.rs
//! measures the Local APIC timer against.

use crate::arch::x86_64::io::inl;
use crate::boot::requests;
use crate::mm::vmm;
use crate::sync::Global;

pub const PM_TIMER_HZ: u32 = 3_579_545;

#[repr(C, packed)]
struct Rsdp {
    signature: [u8; 8], // "RSD PTR "
    checksum: u8,
    oem_id: [u8; 6],
    revision: u8, // 0 = ACPI 1.0 (RSDT only), 2+ = has the XSDT.
    rsdt_address: u32,
    length: u32,
    xsdt_address: u64,
    extended_checksum: u8,
    reserved: [u8; 3],
}

#[repr(C, packed)]
struct Header {
    signature: [u8; 4],
    length: u32, // Including this header.
    revision: u8,
    checksum: u8,
    oem_id: [u8; 6],
    oem_table_id: [u8; 8],
    oem_revision: u32,
    creator_id: u32,
    creator_revision: u32,
}

// FADT field offsets used here (ACPI spec, "Fixed ACPI Description
// Table"); the table is long and only these matter.
const FADT_PM_TMR_BLK: usize = 76; // u32 I/O port
const FADT_FLAGS: usize = 112; // u32
const FADT_X_PM_TMR_BLK: usize = 208; // 12-byte Generic Address Structure
const FADT_FLAG_TMR_VAL_EXT: u32 = 1 << 8; // PM timer is 32 bits, not 24.
const GAS_SPACE_IO: u8 = 1;

struct PmTimer {
    port: u16,
    mask: u32,
}

static PM: Global<PmTimer> = Global::new(PmTimer { port: 0, mask: 0 });

fn pm() -> &'static mut PmTimer {
    unsafe { PM.get() }
}

/// ACPI tables can sit in memory the HHDM doesn't cover (firmware-reserved
/// ranges), so each one gets its own mapping: the header first, to learn
/// the length, then the whole table.
fn map_table(phys: u64) -> Option<&'static [u8]> {
    let header = vmm::map_mmio(phys, size_of::<Header>() as u64) as *const Header;
    let len = unsafe { (*header).length } as usize;
    if len < size_of::<Header>() || len > 0x10_0000 {
        return None;
    }
    let table = vmm::map_mmio(phys, len as u64);
    Some(unsafe { core::slice::from_raw_parts(table, len) })
}

fn u32_at(t: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(t[off..off + 4].try_into().unwrap())
}

fn u64_at(t: &[u8], off: usize) -> u64 {
    u64::from_le_bytes(t[off..off + 8].try_into().unwrap())
}

fn parse_fadt(f: &[u8]) {
    let mut port = 0;
    if f.len() >= FADT_X_PM_TMR_BLK + 12 && f[FADT_X_PM_TMR_BLK] == GAS_SPACE_IO {
        port = u64_at(f, FADT_X_PM_TMR_BLK + 4);
    }
    if port == 0 && f.len() >= FADT_PM_TMR_BLK + 4 {
        port = u32_at(f, FADT_PM_TMR_BLK) as u64;
    }
    if port == 0 || port > 0xFFFF {
        kprintln!("acpi: FADT has no I/O-port PM timer");
        return;
    }
    let ext = f.len() >= FADT_FLAGS + 4 && u32_at(f, FADT_FLAGS) & FADT_FLAG_TMR_VAL_EXT != 0;
    *pm() = PmTimer { port: port as u16, mask: if ext { 0xFFFF_FFFF } else { 0x00FF_FFFF } };
}

/// Finds the RSDP Limine handed over, walks the XSDT (or RSDT on ACPI
/// 1.0) and remembers what's needed from the tables -- so far just the
/// FADT's PM timer. Must run after the PMM is up (tables are mapped
/// through vmm::map_mmio()). True if the FADT was found.
pub fn init() -> bool {
    let Some(rsdp) = requests::rsdp().filter(|r| !r.address.is_null()) else {
        kprintln!("acpi: no RSDP from the bootloader");
        return false;
    };
    // Limine hands over an HHDM pointer here; turn it back into a physical
    // address so every table goes through the same mapping.
    let hhdm = requests::hhdm_offset();
    let addr = rsdp.address as u64;
    let rsdp_phys = if addr >= hhdm { addr - hhdm } else { addr };
    let rsdp = unsafe { (vmm::map_mmio(rsdp_phys, size_of::<Rsdp>() as u64) as *const Rsdp).read_unaligned() };
    if &rsdp.signature != b"RSD PTR " {
        kprintln!("acpi: bad RSDP signature");
        return false;
    }

    let use_xsdt = rsdp.revision >= 2 && rsdp.xsdt_address != 0;
    let name = if use_xsdt { "XSDT" } else { "RSDT" };
    let Some(root) = map_table(if use_xsdt { rsdp.xsdt_address } else { rsdp.rsdt_address as u64 }) else {
        kprintln!("acpi: unreadable {name}");
        return false;
    };

    let entry_size = if use_xsdt { 8 } else { 4 };
    let entries = &root[size_of::<Header>()..];
    let count = entries.len() / entry_size;
    for e in entries.chunks_exact(entry_size) {
        let phys = if use_xsdt { u64_at(e, 0) } else { u32_at(e, 0) as u64 };
        if let Some(table) = map_table(phys)
            && &table[..4] == b"FACP"
        {
            parse_fadt(table);
            kprintln!(
                "acpi: {name} with {count} tables; PM timer {}",
                if pm_timer_present() { "found" } else { "missing" }
            );
            return true;
        }
    }
    kprintln!("acpi: no FADT among {count} tables");
    false
}

/// Whether the FADT describes a usable PM timer.
pub fn pm_timer_present() -> bool {
    pm().port != 0
}

pub fn pm_timer_read() -> u32 {
    let p = pm();
    inl(p.port) & p.mask
}

/// PM timer ticks elapsed between two pm_timer_read() values, handling
/// the counter wrapping (it's 24 bits on some machines).
pub fn pm_timer_delta(from: u32, to: u32) -> u32 {
    to.wrapping_sub(from) & pm().mask
}
