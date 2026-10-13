//! USB sticks and disks: the mass-storage class over Bulk-Only Transport
//! with SCSI commands -- what nearly all of them speak. Each one is
//! registered as a block device named usb0, usb1... (UAS-only devices,
//! mostly fast external SSDs, also offer this.)
//!
//! USB Mass Storage Class, Bulk-Only Transport (rev 1.0): every command is
//! a 31-byte Command Block Wrapper out, the data, then a 13-byte Command
//! Status Wrapper in. The commands themselves are SCSI (SBC-3/SPC-4).

use alloc::boxed::Box;
use alloc::format;
use alloc::string::String;

use super::{CC_SHORT_PACKET, CC_STALL, CC_SUCCESS, UsbDevice};
use crate::drivers::block::{self, BlockDriver};
use crate::drivers::timer;
use crate::mm::dma::{self, Dma};
use crate::mm::pmm::PAGE_SIZE;
use crate::sync::Global;

const MAX_DISKS: usize = 4;
const BOUNCE_BYTES: u32 = 64 * 1024;
const CBW_SIGNATURE: u32 = 0x4342_5355; // "USBC"
const CSW_SIGNATURE: u32 = 0x5342_5355; // "USBS"
const TRANSFER_TIMEOUT_MS: u32 = 10000;

const SCSI_TEST_UNIT_READY: u8 = 0x00;
const SCSI_REQUEST_SENSE: u8 = 0x03;
const SCSI_INQUIRY: u8 = 0x12;
const SCSI_READ_CAPACITY_10: u8 = 0x25;
const SCSI_READ_10: u8 = 0x28;
const SCSI_WRITE_10: u8 = 0x2A;
const SCSI_READ_16: u8 = 0x88;
const SCSI_WRITE_16: u8 = 0x8A;
const SCSI_READ_CAPACITY_16: u8 = 0x9E;

#[repr(C, packed)]
struct Cbw {
    signature: u32,
    tag: u32,
    data_length: u32,
    flags: u8, // 0x80: data comes in
    lun: u8,
    cb_length: u8,
    cb: [u8; 16],
}

#[repr(C, packed)]
struct Csw {
    signature: u32,
    tag: u32,
    residue: u32,
    status: u8, // 0 passed, 1 failed, 2 phase error
}

struct Disk {
    name: String,
    dev: &'static UsbDevice,
    interface: u8,
    ep_in: u8,
    ep_out: u8,
    dci_in: u32,
    dci_out: u32,
    tag: u32,
    /// BOUNCE_BYTES of data, then a page for the CBW (at 0) and CSW (at 64).
    bounce: Dma,
    sector_size: u32,
}

static DISK_COUNT: Global<usize> = Global::new(0);

impl Disk {
    fn data(&self, len: usize) -> &mut [u8] {
        unsafe { core::slice::from_raw_parts_mut(self.bounce.virt, len) }
    }

    fn clear_halt(&self, ep: u8) {
        self.dev.control_nodata(0x02, 0x01, 0, ep as u16); // CLEAR_FEATURE(ENDPOINT_HALT)
    }

    /// Reset Recovery (BOT 5.3.4): reset the interface, then clear the halt
    /// on both pipes -- what's needed after a phase error or a transfer gone
    /// wrong to get the device listening for commands again.
    fn reset_recovery(&self) {
        self.dev.control_nodata(0x21, 0xFF, 0, self.interface as u16); // Bulk-Only Mass Storage Reset
        self.clear_halt(self.ep_in);
        self.clear_halt(self.ep_out);
    }

    /// One SCSI command. Data moves through the bounce buffer; `input` says
    /// which way. True if the device reports success.
    fn scsi(&mut self, cb: &[u8], length: u32, input: bool) -> bool {
        let small = unsafe { self.bounce.virt.add(BOUNCE_BYTES as usize) };
        let small_phys = self.bounce.phys + BOUNCE_BYTES as u64;
        self.tag = self.tag.wrapping_add(1);
        let mut cbw = Cbw {
            signature: CBW_SIGNATURE,
            tag: self.tag,
            data_length: length,
            flags: if input { 0x80 } else { 0 },
            lun: 0,
            cb_length: cb.len() as u8,
            cb: [0; 16],
        };
        cbw.cb[..cb.len()].copy_from_slice(cb);
        unsafe { small.cast::<Cbw>().write_volatile(cbw) };

        let (cc, _) = self.dev.bulk(self.dci_out, small_phys, size_of::<Cbw>() as u32, TRANSFER_TIMEOUT_MS);
        if cc != CC_SUCCESS {
            self.reset_recovery();
            return false;
        }
        if length > 0 {
            let dci = if input { self.dci_in } else { self.dci_out };
            let (cc, _) = self.dev.bulk(dci, self.bounce.phys, length, TRANSFER_TIMEOUT_MS);
            if cc == CC_STALL {
                // Stalled data stage: clear it on the device; the CSW follows.
                self.clear_halt(if input { self.ep_in } else { self.ep_out });
            } else if cc != CC_SUCCESS && cc != CC_SHORT_PACKET {
                self.reset_recovery();
                return false;
            }
        }

        let csw = unsafe { small.add(64).cast::<Csw>() };
        let csw_phys = small_phys + 64;
        unsafe { core::ptr::write_bytes(csw, 0, 1) };
        let mut cc = self.dev.bulk(self.dci_in, csw_phys, size_of::<Csw>() as u32, TRANSFER_TIMEOUT_MS).0;
        if cc == CC_STALL {
            self.clear_halt(self.ep_in);
            cc = self.dev.bulk(self.dci_in, csw_phys, size_of::<Csw>() as u32, TRANSFER_TIMEOUT_MS).0;
        }
        let s = unsafe { csw.read_volatile() };
        let (signature, tag, status) = (s.signature, s.tag, s.status);
        if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) || signature != CSW_SIGNATURE || tag != self.tag || status == 2 {
            self.reset_recovery();
            return false;
        }
        status == 0
    }

    fn rw(&mut self, lba: u64, count: u32, write: bool) -> bool {
        let mut cb = [0u8; 16];
        let len = if lba + count as u64 <= 0xFFFF_FFFF {
            cb[0] = if write { SCSI_WRITE_10 } else { SCSI_READ_10 };
            cb[2..6].copy_from_slice(&(lba as u32).to_be_bytes());
            cb[7..9].copy_from_slice(&(count as u16).to_be_bytes());
            10
        } else {
            cb[0] = if write { SCSI_WRITE_16 } else { SCSI_READ_16 };
            cb[2..10].copy_from_slice(&lba.to_be_bytes());
            cb[10..14].copy_from_slice(&count.to_be_bytes());
            16
        };
        if !self.scsi(&cb[..len], count * self.sector_size, !write) {
            kprintln!(
                "usb-storage: {}: {} of {count} sectors at {lba} failed",
                self.name,
                if write { "write" } else { "read" }
            );
            return false;
        }
        true
    }

    /// Asks for the medium until it's ready -- sticks and card readers can
    /// take a moment after being configured.
    fn wait_ready(&mut self) -> bool {
        for _ in 0..20 {
            if self.scsi(&[SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0], 0, false) {
                return true;
            }
            // Clears the "unit attention" a fresh device reports.
            self.scsi(&[SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0], 18, true);
            timer::sleep_ms(100);
        }
        false
    }

    /// INQUIRY, then capacity: (model, sector count).
    fn start(&mut self) -> Option<(String, u64)> {
        let port = self.dev.port;
        if !self.scsi(&[SCSI_INQUIRY, 0, 0, 0, 36, 0], 36, true) {
            kprintln!("usb-storage: port {port}: INQUIRY failed");
            return None;
        }
        let inquiry = self.data(36);
        if inquiry[0] & 0x1F != 0 {
            kprintln!("usb-storage: port {port}: not a disk (SCSI type {}) -- skipped", inquiry[0] & 0x1F);
            return None;
        }
        let trimmed = |b: &[u8]| String::from(String::from_utf8_lossy(b).trim_end_matches([' ', '\0']));
        let model = format!("{} {}", trimmed(&inquiry[8..16]), trimmed(&inquiry[16..32]));

        if !self.wait_ready() {
            kprintln!("usb-storage: port {port}: no medium, or it never became ready");
            return None;
        }

        if !self.scsi(&[SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0], 8, true) {
            kprintln!("usb-storage: port {port}: READ CAPACITY failed");
            return None;
        }
        let be32 = |b: &[u8]| u32::from_be_bytes(b[..4].try_into().unwrap());
        let cap = self.data(8);
        let (mut last, mut sector_size) = (be32(cap) as u64, be32(&cap[4..]));
        if last == 0xFFFF_FFFF {
            // Over 2 TiB: ask again with 64-bit LBAs.
            let mut cb = [0u8; 16];
            cb[0] = SCSI_READ_CAPACITY_16;
            cb[1] = 0x10;
            cb[13] = 32;
            if !self.scsi(&cb, 32, true) {
                return None;
            }
            let cap = self.data(12);
            last = u64::from_be_bytes(cap[..8].try_into().unwrap());
            sector_size = be32(&cap[8..]);
        }
        if ![512, 2048, 4096].contains(&sector_size) {
            kprintln!("usb-storage: port {port}: unusable sector size {sector_size}");
            return None;
        }
        self.sector_size = sector_size;
        Some((model, last + 1))
    }
}

impl BlockDriver for Disk {
    fn read(&mut self, lba: u64, count: u32, buf: &mut [u8]) -> bool {
        if !self.rw(lba, count, false) {
            return false;
        }
        buf.copy_from_slice(self.data(buf.len()));
        true
    }

    fn write(&mut self, lba: u64, count: u32, buf: &[u8]) -> bool {
        self.data(buf.len()).copy_from_slice(buf);
        self.rw(lba, count, true)
    }
}

/// Called for each newly enumerated device that no other class driver
/// claimed. True if it took it.
pub fn probe(dev: &'static UsbDevice, config: &[u8]) -> bool {
    let disk_count = unsafe { DISK_COUNT.get() };
    if *disk_count == MAX_DISKS {
        return false;
    }
    // A mass-storage interface (class 8) speaking SCSI (subclass 6) over
    // Bulk-Only Transport (protocol 0x50), and its two bulk endpoints.
    let mut in_storage = false;
    let (mut interface, mut ep_in, mut ep_out, mut mps_in, mut mps_out) = (0u8, 0u8, 0u8, 0u16, 0u16);
    let mut off = 0;
    while off + 2 <= config.len() && config[off] >= 2 {
        let d = &config[off..];
        if off + d[0] as usize > config.len() {
            break;
        }
        if d[1] == 4 && d[0] >= 9 {
            if ep_in != 0 && ep_out != 0 {
                break;
            }
            in_storage = d[5] == 8 && d[6] == 6 && d[7] == 0x50;
            interface = d[2];
        } else if d[1] == 5 && d[0] >= 7 && in_storage && d[3] & 3 == 2 {
            let mps = u16::from_le_bytes([d[4], d[5]]) & 0x7FF;
            if d[2] & 0x80 != 0 {
                (ep_in, mps_in) = (d[2], mps);
            } else {
                (ep_out, mps_out) = (d[2], mps);
            }
        }
        off += d[0] as usize;
    }
    if ep_in == 0 || ep_out == 0 {
        return false;
    }

    if !dev.control_nodata(0x00, 9, config[5] as u16, 0) {
        // SET_CONFIGURATION
        kprintln!("usb-storage: port {}: SET_CONFIGURATION failed", dev.port);
        return false;
    }
    let (Some(dci_in), Some(dci_out), Some(bounce)) =
        (dev.add_bulk(ep_in, mps_in), dev.add_bulk(ep_out, mps_out), dma::alloc(BOUNCE_BYTES as u64 / PAGE_SIZE + 1))
    else {
        return false;
    };
    let name = format!("usb{}", *disk_count);
    let mut d =
        Disk { name: name.clone(), dev, interface, ep_in, ep_out, dci_in, dci_out, tag: 0, bounce, sector_size: 0 };
    let Some((model, sectors)) = d.start() else { return false };
    kprintln!("usb-storage: port {}: {model} registered as {name}", dev.port);
    let sector_size = d.sector_size;
    block::register(name, model, sector_size, sectors, BOUNCE_BYTES / sector_size, Box::new(d));
    *disk_count += 1;
    true
}
