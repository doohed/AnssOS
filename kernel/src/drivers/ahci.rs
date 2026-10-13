//! SATA disks through AHCI controllers (PCI class 01.06, prog-if 01;
//! QEMU's q35 machine has one built in). Each port with a disk on it is
//! registered as a block device named sata0, sata1...; CD/DVD drives and
//! port multipliers are skipped. Polled, one command at a time. Taking a
//! port over and identifying its drive doesn't touch the drive's data.
//!
//! Serial ATA AHCI 1.3.1 (HBA and port registers, section 3; command lists
//! and tables, 4) and ATA8-ACS (IDENTIFY DEVICE, READ/WRITE DMA EXT).

use alloc::boxed::Box;
use alloc::format;
use alloc::string::String;
use core::sync::atomic::{Ordering, fence};

use super::block::{self, BlockDriver};
use super::{pci, timer};
use crate::mm::dma::{self, Dma};
use crate::mm::pmm::PAGE_SIZE;
use crate::mm::vmm;
use crate::mmio::Regs;

const MAX_DISKS: usize = 8;
const BOUNCE_BYTES: u32 = 64 * 1024;
const COMMAND_TIMEOUT_MS: u64 = 5000;

const HBA_GHC: u32 = 0x04;
const HBA_PI: u32 = 0x0C;
const HBA_CAP2: u32 = 0x24;
const HBA_BOHC: u32 = 0x28;
const GHC_AHCI_ENABLE: u32 = 1 << 31;

const fn port(p: u32) -> u32 {
    0x100 + 0x80 * p
}
const PX_CLB: u32 = 0x00;
const PX_FB: u32 = 0x08;
const PX_IS: u32 = 0x10;
const PX_IE: u32 = 0x14;
const PX_CMD: u32 = 0x18;
const PX_TFD: u32 = 0x20;
const PX_SIG: u32 = 0x24;
const PX_SSTS: u32 = 0x28;
const PX_SERR: u32 = 0x30;
const PX_CI: u32 = 0x38;

const CMD_ST: u32 = 1 << 0;
const CMD_FRE: u32 = 1 << 4;
const CMD_FR: u32 = 1 << 14;
const CMD_CR: u32 = 1 << 15;
const TFD_ERR: u32 = 0x01;
const TFD_DRQ: u32 = 0x08;
const TFD_BSY: u32 = 0x80;
const IS_TFES: u32 = 1 << 30;

const SIG_ATA: u32 = 0x0000_0101;
const SIG_ATAPI: u32 = 0xEB14_0101;

const ATA_IDENTIFY: u8 = 0xEC;
const ATA_READ_DMA_EXT: u8 = 0x25;
const ATA_WRITE_DMA_EXT: u8 = 0x35;

#[repr(C)]
struct CommandHeader {
    flags: u16, // FIS length in dwords (4:0), write (6).
    prdt_length: u16,
    bytes_done: u32,
    table: u64,
    reserved: [u32; 4],
}

#[repr(C)]
struct PrdtEntry {
    address: u64,
    reserved: u32,
    count: u32, // Bytes - 1 in 21:0.
}

#[repr(C)]
struct CommandTable {
    fis: [u8; 64],
    atapi: [u8; 16],
    reserved: [u8; 48],
    prdt: [PrdtEntry; 1],
}

struct Disk {
    name: String,
    /// This port's registers.
    port: Regs,
    list: *mut CommandHeader,
    table: *mut CommandTable,
    bounce: Dma,
}

fn wait_clear(regs: Regs, off: u32, mask: u32, ms: u32) -> bool {
    for _ in 0..=ms {
        if regs.rd32(off) & mask == 0 {
            return true;
        }
        timer::sleep_ms(1);
    }
    false
}

impl Disk {
    /// Issues one command in slot 0 and waits for it. `bytes` of data move
    /// through the bounce buffer.
    fn run(&mut self, command: u8, lba: u64, count: u16, bytes: u32, write: bool) -> bool {
        let p = self.port;
        if !wait_clear(p, PX_TFD, TFD_BSY | TFD_DRQ, 1000) {
            kprintln!("ahci: {}: drive stays busy", self.name);
            return false;
        }
        let mut fis = [0u8; 20];
        fis[0] = 0x27; // Register FIS, host to device
        fis[1] = 0x80; // ...carrying a command
        fis[2] = command;
        fis[4..7].copy_from_slice(&lba.to_le_bytes()[0..3]);
        fis[7] = 0x40; // LBA mode
        fis[8..11].copy_from_slice(&lba.to_le_bytes()[3..6]);
        fis[12..14].copy_from_slice(&count.to_le_bytes());
        unsafe {
            (&raw mut (*self.table).fis).cast::<[u8; 20]>().write_volatile(fis);
            (&raw mut (*self.table).prdt[0]).write_volatile(PrdtEntry {
                address: self.bounce.phys,
                reserved: 0,
                count: bytes - 1,
            });
        }
        vwrite!(self.list, flags, 5 | if write { 1 << 6 } else { 0 }); // 5-dword FIS
        vwrite!(self.list, prdt_length, 1);
        vwrite!(self.list, bytes_done, 0);

        p.wr32(PX_IS, 0xFFFF_FFFF);
        fence(Ordering::SeqCst);
        p.wr32(PX_CI, 1);

        let deadline = timer::uptime_ms() + COMMAND_TIMEOUT_MS;
        for spins in 0u32.. {
            if p.rd32(PX_IS) & IS_TFES != 0 {
                kprintln!("ahci: {}: command 0x{command:x} failed (status 0x{:x})", self.name, p.rd32(PX_TFD));
                return false;
            }
            if p.rd32(PX_CI) & 1 == 0 {
                break;
            }
            if spins > 100_000 {
                if timer::uptime_ms() > deadline {
                    kprintln!("ahci: {}: command 0x{command:x} timed out", self.name);
                    return false;
                }
                timer::idle();
            }
        }
        p.rd32(PX_TFD) & TFD_ERR == 0
    }

    fn bounce(&self, len: usize) -> &mut [u8] {
        unsafe { core::slice::from_raw_parts_mut(self.bounce.virt, len) }
    }
}

impl BlockDriver for Disk {
    fn read(&mut self, lba: u64, count: u32, buf: &mut [u8]) -> bool {
        if !self.run(ATA_READ_DMA_EXT, lba, count as u16, buf.len() as u32, false) {
            return false;
        }
        buf.copy_from_slice(self.bounce(buf.len()));
        true
    }

    fn write(&mut self, lba: u64, count: u32, buf: &[u8]) -> bool {
        self.bounce(buf.len()).copy_from_slice(buf);
        self.run(ATA_WRITE_DMA_EXT, lba, count as u16, buf.len() as u32, true)
    }
}

/// IDENTIFY strings are big-endian within each 16-bit word.
fn ata_string(words: &[u16]) -> String {
    let bytes: alloc::vec::Vec<u8> = words.iter().flat_map(|w| w.to_be_bytes()).collect();
    String::from_utf8_lossy(&bytes).trim_end_matches([' ', '\0']).into()
}

/// Takes over one port with a drive on it: stop its engines, give it our
/// command list and FIS area, start again, then IDENTIFY. Nothing here
/// touches the drive's data. (driver, model, sector size, sector count).
fn start_port(port: Regs, index: u32, name: String) -> Option<(Disk, String, u32, u64)> {
    port.wr32(PX_CMD, port.rd32(PX_CMD) & !CMD_ST);
    if !wait_clear(port, PX_CMD, CMD_CR, 500) {
        return None;
    }
    port.wr32(PX_CMD, port.rd32(PX_CMD) & !CMD_FRE);
    if !wait_clear(port, PX_CMD, CMD_FR, 500) {
        return None;
    }

    let page = dma::alloc(1)?; // Command list at 0, received FISes at 1 KiB.
    let table = dma::alloc(1)?;
    let bounce = dma::alloc(BOUNCE_BYTES as u64 / PAGE_SIZE)?;
    let list: *mut CommandHeader = page.as_ptr();
    vwrite!(list, table, table.phys);
    port.wr64(PX_CLB, page.phys);
    port.wr64(PX_FB, page.phys + 1024);
    port.wr32(PX_SERR, 0xFFFF_FFFF);
    port.wr32(PX_IS, 0xFFFF_FFFF);
    port.wr32(PX_IE, 0); // Polled.
    port.wr32(PX_CMD, port.rd32(PX_CMD) | CMD_FRE);
    port.wr32(PX_CMD, port.rd32(PX_CMD) | CMD_ST);

    let mut d = Disk { name, port, list, table: table.as_ptr(), bounce };
    if !d.run(ATA_IDENTIFY, 0, 0, 512, false) {
        kprintln!("ahci: port {index}: IDENTIFY failed");
        return None;
    }
    let id: &[u16] = unsafe { core::slice::from_raw_parts(d.bounce.as_ptr(), 256) };
    if id[83] & 1 << 10 == 0 {
        kprintln!("ahci: port {index}: drive has no 48-bit addressing -- skipped");
        return None;
    }
    let sectors = id[100] as u64 | (id[101] as u64) << 16 | (id[102] as u64) << 32 | (id[103] as u64) << 48;
    let mut sector_size = 512;
    if id[106] & 0xC000 == 0x4000 && id[106] & 1 << 12 != 0 {
        sector_size = (id[117] as u32 | (id[118] as u32) << 16) * 2;
    }
    if sectors == 0 || (sector_size != 512 && sector_size != 4096) {
        kprintln!("ahci: port {index}: unusable geometry");
        return None;
    }
    let model = ata_string(&id[27..47]);
    Some((d, model, sector_size, sectors))
}

/// AHCI's BIOS/OS handoff (10.6), when the controller supports it.
fn bios_handoff(hba: Regs) {
    if hba.rd32(HBA_CAP2) & 1 == 0 {
        return;
    }
    hba.wr32(HBA_BOHC, hba.rd32(HBA_BOHC) | 1 << 1); // OS owned
    wait_clear(hba, HBA_BOHC, 1 << 0, 50); // BIOS owned
    if hba.rd32(HBA_BOHC) & 1 << 4 != 0 {
        wait_clear(hba, HBA_BOHC, 1 << 4, 2000); // BIOS busy
    }
}

/// Registers every SATA disk on every AHCI controller. How many.
pub fn init() -> i32 {
    let mut count = 0;
    for i in 0.. {
        let Some(pci) = pci::find_class_nth(0x01, 0x06, 0x01, i) else { break };
        pci.write16(0x04, pci.read16(0x04) | 0x0406);
        let hba = Regs(vmm::map_mmio((pci.bar[5] & !0xF) as u64, port(32) as u64));

        bios_handoff(hba);
        hba.wr32(HBA_GHC, hba.rd32(HBA_GHC) | GHC_AHCI_ENABLE);
        let implemented = hba.rd32(HBA_PI);
        kprintln!(
            "ahci: controller at {}:{}.{} ({:x}:{:x})",
            pci.bus,
            pci.slot,
            pci.func,
            pci.vendor_id,
            pci.device_id
        );

        for p in 0..32 {
            if count == MAX_DISKS {
                break;
            }
            if implemented & 1 << p == 0 {
                continue;
            }
            let regs = hba.at(port(p));
            if regs.rd32(PX_SSTS) & 0xF != 3 {
                continue; // Nothing attached.
            }
            let sig = regs.rd32(PX_SIG);
            if sig != SIG_ATA {
                kprintln!(
                    "ahci: port {p}: {} -- skipped",
                    if sig == SIG_ATAPI { "CD/DVD drive" } else { "not a disk" }
                );
                continue;
            }
            let name = format!("sata{count}");
            let Some((disk, model, sector_size, sectors)) = start_port(regs, p, name.clone()) else { continue };
            block::register(name, model, sector_size, sectors, BOUNCE_BYTES / sector_size, Box::new(disk));
            count += 1;
        }
    }
    count as i32
}
