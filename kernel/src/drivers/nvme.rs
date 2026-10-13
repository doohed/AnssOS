//! NVMe SSDs (PCI class 01.08, prog-if 02; QEMU: -device nvme). Brings
//! each controller up with one I/O queue pair, polled, and registers
//! namespace 1 as a block device named nvme0, nvme1... Bringing a
//! controller up only resets it and asks it questions; the drive's data
//! is only touched by block reads and writes.
//!
//! NVM Express base specification (rev 1.4): controller registers in
//! section 3.1, queues and commands in 4 and 5, I/O commands in the NVM
//! command set.

use alloc::boxed::Box;
use alloc::format;
use alloc::string::String;
use core::sync::atomic::{Ordering, fence};

use super::block::{self, BlockDriver};
use super::pci::{self, PciDevice};
use super::timer;
use crate::mm::dma::{self, Dma};
use crate::mm::pmm::PAGE_SIZE;
use crate::mm::vmm;
use crate::mmio::Regs;

const MAX_CONTROLLERS: usize = 4;
/// One page of 64-byte submission entries.
const QUEUE_ENTRIES: u32 = 64;
const BOUNCE_BYTES: u32 = 64 * 1024;
const COMMAND_TIMEOUT_MS: u64 = 3000;

const REG_CAP: u32 = 0x00;
const REG_INTMS: u32 = 0x0C;
const REG_CC: u32 = 0x14;
const REG_CSTS: u32 = 0x1C;
const REG_AQA: u32 = 0x24;
const REG_ASQ: u32 = 0x28;
const REG_ACQ: u32 = 0x30;

const CC_ENABLE: u32 = 1;
const CC_IOSQES: u32 = 6 << 16; // 64-byte submission entries
const CC_IOCQES: u32 = 4 << 20; // 16-byte completion entries
const CSTS_READY: u32 = 1;
const CSTS_FATAL: u32 = 2;

const ADMIN_CREATE_SQ: u32 = 0x01;
const ADMIN_CREATE_CQ: u32 = 0x05;
const ADMIN_IDENTIFY: u32 = 0x06;
const IO_WRITE: u32 = 0x01;
const IO_READ: u32 = 0x02;

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct SqEntry {
    cdw0: u32, // Opcode; command ID in 31:16.
    nsid: u32,
    reserved: u64,
    metadata: u64,
    prp1: u64,
    prp2: u64,
    cdw10: u32,
    cdw11: u32,
    cdw12: u32,
    cdw13: u32,
    cdw14: u32,
    cdw15: u32,
}

#[repr(C)]
struct CqEntry {
    result: u32,
    reserved: u32,
    sq_head: u16,
    sq_id: u16,
    command_id: u16,
    status: u16, // Phase tag in bit 0.
}

struct Queue {
    sq: Dma,
    cq: Dma,
    sq_tail: u32,
    cq_head: u32,
    phase: u16,
    sq_doorbell: *mut u32,
    cq_doorbell: *mut u32,
}

struct Nvme {
    admin: Queue,
    io: Option<Queue>,
    next_id: u16,
    nsid: u32,
    bounce: Dma,
    /// For transfers over two pages.
    prp_list: Dma,
    sector_size: u32,
}

/// Waits up to `timeout_ms` for CSTS.RDY to read `want`.
fn wait_ready(regs: Regs, want: u32, timeout_ms: u32) -> bool {
    for _ in (0..=timeout_ms).step_by(10) {
        let csts = regs.rd32(REG_CSTS);
        if csts == 0xFFFF_FFFF {
            return false; // The device fell off the bus.
        }
        if csts & CSTS_READY == want {
            return true;
        }
        timer::sleep_ms(10);
    }
    false
}

/// A submission/completion queue pair with id `qid`; `stride` is the
/// doorbell spacing.
fn queue(regs: Regs, qid: u32, stride: u32) -> Option<Queue> {
    let doorbell = |n: u32| unsafe { regs.0.add(0x1000 + (n * stride) as usize).cast::<u32>() };
    Some(Queue {
        sq: dma::alloc(1)?,
        cq: dma::alloc(1)?,
        sq_tail: 0,
        cq_head: 0,
        phase: 1,
        sq_doorbell: doorbell(2 * qid),
        cq_doorbell: doorbell(2 * qid + 1),
    })
}

impl Nvme {
    /// Submits one command and polls its completion. The NVMe status
    /// (0 = success), or None on timeout.
    fn submit(&mut self, io: bool, mut cmd: SqEntry) -> Option<u16> {
        let id = self.next_id;
        self.next_id = self.next_id.wrapping_add(1);
        let q = if io { self.io.as_mut().unwrap() } else { &mut self.admin };
        cmd.cdw0 = cmd.cdw0 & 0xFFFF | (id as u32) << 16;
        unsafe { q.sq.as_ptr::<SqEntry>().add(q.sq_tail as usize).write_volatile(cmd) };
        q.sq_tail = (q.sq_tail + 1) % QUEUE_ENTRIES;
        fence(Ordering::SeqCst);
        unsafe { q.sq_doorbell.write_volatile(q.sq_tail) };

        let deadline = timer::uptime_ms() + COMMAND_TIMEOUT_MS;
        for spins in 0u32.. {
            let e = unsafe { q.cq.as_ptr::<CqEntry>().add(q.cq_head as usize) };
            let status = vread!(e, status);
            if status & 1 == q.phase {
                let got = vread!(e, command_id);
                q.cq_head = (q.cq_head + 1) % QUEUE_ENTRIES;
                if q.cq_head == 0 {
                    q.phase ^= 1;
                }
                unsafe { q.cq_doorbell.write_volatile(q.cq_head) };
                if got == id {
                    return Some(status >> 1);
                }
                continue; // A stale completion; keep looking for ours.
            }
            if spins > 100_000 {
                if timer::uptime_ms() > deadline {
                    return None;
                }
                timer::idle();
            }
        }
        unreachable!()
    }

    /// Points the command at the bounce buffer: PRP1 is its first page,
    /// PRP2 the second page or, past two pages, a list of the rest (4.3).
    fn set_prps(&self, cmd: &mut SqEntry, bytes: u32) {
        let pages = bytes.div_ceil(PAGE_SIZE as u32) as u64;
        cmd.prp1 = self.bounce.phys;
        if pages == 2 {
            cmd.prp2 = self.bounce.phys + PAGE_SIZE;
        } else if pages > 2 {
            for i in 1..pages {
                unsafe {
                    self.prp_list.as_ptr::<u64>().add(i as usize - 1).write_volatile(self.bounce.phys + i * PAGE_SIZE)
                };
            }
            cmd.prp2 = self.prp_list.phys;
        }
    }

    fn io(&mut self, opcode: u32, lba: u64, count: u32) -> bool {
        let mut cmd = SqEntry { cdw0: opcode, nsid: self.nsid, ..Default::default() };
        self.set_prps(&mut cmd, count * self.sector_size);
        cmd.cdw10 = lba as u32;
        cmd.cdw11 = (lba >> 32) as u32;
        cmd.cdw12 = count - 1;
        match self.submit(true, cmd) {
            Some(0) => true,
            status => {
                let what = if opcode == IO_READ { "read" } else { "write" };
                kprintln!(
                    "nvme: {what} of {count} sectors at {lba} failed (status 0x{:x})",
                    status.map_or(0xFFFF_FFFF, u32::from)
                );
                false
            }
        }
    }

    fn identify(&mut self, cns: u32, nsid: u32) -> bool {
        self.submit(
            false,
            SqEntry { cdw0: ADMIN_IDENTIFY, nsid, prp1: self.bounce.phys, cdw10: cns, ..Default::default() },
        ) == Some(0)
    }

    fn bounce(&self, len: usize) -> &mut [u8] {
        unsafe { core::slice::from_raw_parts_mut(self.bounce.virt, len) }
    }
}

impl BlockDriver for Nvme {
    fn read(&mut self, lba: u64, count: u32, buf: &mut [u8]) -> bool {
        if !self.io(IO_READ, lba, count) {
            return false;
        }
        buf.copy_from_slice(self.bounce(buf.len()));
        true
    }

    fn write(&mut self, lba: u64, count: u32, buf: &[u8]) -> bool {
        self.bounce(buf.len()).copy_from_slice(buf);
        self.io(IO_WRITE, lba, count)
    }
}

/// An Identify string field: space-padded, not NUL-terminated.
fn trimmed(field: &[u8]) -> String {
    String::from_utf8_lossy(field).trim_matches([' ', '\0']).into()
}

/// Brings one controller up; (driver, model, sector count, max sectors
/// per transfer).
fn start(pci: &PciDevice) -> Option<(Nvme, String, u64, u32)> {
    pci.write16(0x04, pci.read16(0x04) | 0x0406);
    let mut bar = (pci.bar[0] & !0xF) as u64;
    if pci.bar[0] & 0x6 == 0x4 {
        bar |= (pci.bar[1] as u64) << 32;
    }
    let regs = Regs(vmm::map_mmio(bar, 0x2000));

    let cap = regs.rd64(REG_CAP);
    let timeout_ms = ((cap >> 24) & 0xFF) as u32 * 500 + 500;
    let stride = 4u32 << ((cap >> 32) & 0xF);
    if (cap >> 48) & 0xF != 0 {
        kprintln!("nvme: controller can't use 4 KiB pages");
        return None;
    }

    // Disable (a controller reset -- it touches no data), set up the admin
    // queues, enable.
    regs.wr32(REG_CC, regs.rd32(REG_CC) & !CC_ENABLE);
    if !wait_ready(regs, 0, timeout_ms) {
        kprintln!("nvme: controller won't disable");
        return None;
    }
    let admin = queue(regs, 0, stride)?;
    regs.wr32(REG_AQA, (QUEUE_ENTRIES - 1) << 16 | (QUEUE_ENTRIES - 1));
    regs.wr64(REG_ASQ, admin.sq.phys);
    regs.wr64(REG_ACQ, admin.cq.phys);
    regs.wr32(REG_INTMS, 0xFFFF_FFFF); // Polled.
    regs.wr32(REG_CC, CC_IOSQES | CC_IOCQES | CC_ENABLE);
    if !wait_ready(regs, 1, timeout_ms) || regs.rd32(REG_CSTS) & CSTS_FATAL != 0 {
        kprintln!("nvme: controller won't enable");
        return None;
    }

    let mut c = Nvme {
        admin,
        io: None,
        next_id: 0,
        nsid: 0,
        bounce: dma::alloc(BOUNCE_BYTES as u64 / PAGE_SIZE)?,
        prp_list: dma::alloc(1)?,
        sector_size: 0,
    };
    if !c.identify(1, 0) {
        kprintln!("nvme: Identify Controller failed"); // CNS 1: the controller
        return None;
    }
    let model = trimmed(&c.bounce(64)[24..64]);
    let mdts = c.bounce(78)[77] as u32;
    let mut max_bytes = BOUNCE_BYTES;
    if mdts != 0 && mdts < 16 && (PAGE_SIZE as u32) << mdts < max_bytes {
        max_bytes = (PAGE_SIZE as u32) << mdts;
    }

    c.nsid = 1;
    if !c.identify(0, c.nsid) {
        kprintln!("nvme: Identify Namespace failed"); // CNS 0: namespace 1
        return None;
    }
    let id = c.bounce(4096);
    let sectors = u64::from_le_bytes(id[0..8].try_into().unwrap());
    let format = (id[26] & 0xF) as usize;
    let lbads = id[128 + format * 4 + 2] as u32;
    if sectors == 0 || !(9..=12).contains(&lbads) {
        kprintln!("nvme: namespace 1 is unusable ({sectors} sectors, 2^{lbads} bytes each)");
        return None;
    }

    // One I/O queue pair.
    let io = queue(regs, 1, stride)?;
    let (cq_phys, sq_phys) = (io.cq.phys, io.sq.phys);
    c.io = Some(io);
    let create_cq = SqEntry {
        cdw0: ADMIN_CREATE_CQ,
        prp1: cq_phys,
        cdw10: (QUEUE_ENTRIES - 1) << 16 | 1,
        cdw11: 1, // Physically contiguous, interrupts off.
        ..Default::default()
    };
    if c.submit(false, create_cq) != Some(0) {
        kprintln!("nvme: creating the I/O completion queue failed");
        return None;
    }
    let create_sq = SqEntry {
        cdw0: ADMIN_CREATE_SQ,
        prp1: sq_phys,
        cdw10: (QUEUE_ENTRIES - 1) << 16 | 1,
        cdw11: 1 << 16 | 1, // Completes to CQ 1, physically contiguous.
        ..Default::default()
    };
    if c.submit(false, create_sq) != Some(0) {
        kprintln!("nvme: creating the I/O submission queue failed");
        return None;
    }

    c.sector_size = 1 << lbads;
    let max_sectors = max_bytes / c.sector_size;
    Some((c, model, sectors, max_sectors))
}

/// Brings every controller up and registers its namespace 1. How many it
/// registered.
pub fn init() -> i32 {
    let mut count = 0;
    for i in 0.. {
        if count == MAX_CONTROLLERS {
            break;
        }
        let Some(pci) = pci::find_class_nth(0x01, 0x08, 0x02, i) else { break };
        kprintln!(
            "nvme: controller at {}:{}.{} ({:x}:{:x})",
            pci.bus,
            pci.slot,
            pci.func,
            pci.vendor_id,
            pci.device_id
        );
        let Some((c, model, sectors, max_sectors)) = start(pci) else { continue };
        let sector_size = c.sector_size;
        block::register(format!("nvme{count}"), model, sector_size, sectors, max_sectors, Box::new(c));
        count += 1;
    }
    count as i32
}
