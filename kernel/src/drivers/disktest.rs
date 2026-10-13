//! Storage driver checks for the kernel shell's `disktest`.
//!
//! Read check (always safe): reads the first MiB twice and compares, then
//! times a sequential read of up to 64 MiB.
//!
//! Write check (only with allow-disk-writes): in a 1 MiB region in the
//! middle of the disk, saves what's there, writes a pattern, reads it back
//! and compares, then writes the original back and verifies that too --
//! so even the write check leaves the disk as it was.

use alloc::vec;

use super::block::{self, BlockDevice};
use super::timer;

const CHUNK: usize = 1024 * 1024;
const SPEED_BYTES: u64 = 64 * 1024 * 1024;

fn pattern(i: usize) -> u8 {
    (i * 7 + (i >> 9) + 0x5A) as u8
}

fn read_check(dev: &mut BlockDevice, a: &mut [u8], b: &mut [u8]) -> bool {
    let sector = dev.sector_size as u64;
    let sectors = (CHUNK as u64 / sector).min(dev.sector_count);
    let bytes = (sectors * sector) as usize;
    if !dev.read(0, sectors, &mut a[..bytes]) || !dev.read(0, sectors, &mut b[..bytes]) {
        kprintln!("disktest: {}: read failed", dev.name);
        return false;
    }
    if a[..bytes] != b[..bytes] {
        kprintln!("disktest: {}: two reads of the same sectors DIFFER", dev.name);
        return false;
    }
    kprintln!("disktest: {}: first {} KiB read twice, identical", dev.name, bytes / 1024);

    let total = dev.sector_count * sector;
    let bytes = if total < SPEED_BYTES { total - total % CHUNK as u64 } else { SPEED_BYTES };
    if bytes == 0 {
        return true;
    }
    let start = timer::uptime_ms();
    for done in (0..bytes).step_by(CHUNK) {
        if !dev.read(done / sector, CHUNK as u64 / sector, a) {
            kprintln!("disktest: {}: read failed at byte {done}", dev.name);
            return false;
        }
    }
    let ms = timer::uptime_ms() - start;
    kprint!("disktest: {}: read {} MiB in {ms} ms", dev.name, bytes >> 20);
    if ms > 0 {
        kprint!(" ({} MiB/s)", (bytes >> 20) * 1000 / ms);
    }
    kprintln!();
    true
}

fn write_check(dev: &mut BlockDevice, saved: &mut [u8], buf: &mut [u8]) -> bool {
    let sectors = CHUNK as u64 / dev.sector_size as u64;
    if dev.sector_count < 4 * sectors {
        kprintln!("disktest: {}: too small for the write check", dev.name);
        return false;
    }
    let lba = (dev.sector_count / 2) & !(sectors - 1);
    if !dev.read(lba, sectors, saved) {
        kprintln!("disktest: {}: reading the region to save it failed", dev.name);
        return false;
    }
    buf.iter_mut().enumerate().for_each(|(i, b)| *b = pattern(i));
    if !dev.write(lba, sectors, buf) {
        kprintln!("disktest: {}: write failed", dev.name);
        return false;
    }
    buf.fill(0);
    let ok = dev.read(lba, sectors, buf) && buf.iter().enumerate().all(|(i, &b)| b == pattern(i));
    kprintln!(
        "disktest: {}: wrote a 1 MiB pattern at sector {lba}, read back {}",
        dev.name,
        if ok { "identical" } else { "DIFFERENT" }
    );

    let restored = dev.write(lba, sectors, saved) && dev.read(lba, sectors, buf) && buf == saved;
    kprintln!(
        "disktest: {}: original data {}",
        dev.name,
        if restored { "restored and verified" } else { "NOT RESTORED" }
    );
    ok && restored
}

/// True if everything matched.
pub fn run(dev: &mut BlockDevice, write: bool) -> bool {
    let (mut a, mut b) = (vec![0u8; CHUNK], vec![0u8; CHUNK]);
    if !write {
        return read_check(dev, &mut a, &mut b);
    }
    if !block::writes_allowed() {
        kprintln!("disktest: writes are locked (boot with allow-disk-writes to test them)");
        return false;
    }
    write_check(dev, &mut a, &mut b)
}
