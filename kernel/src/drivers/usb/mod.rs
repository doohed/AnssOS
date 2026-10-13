//! USB on xHCI, the controller every PC from the last decade has (QEMU:
//! -device qemu-xhci). Polled, like every other driver here: nothing uses
//! interrupts, poll() drains each controller's event ring.
//!
//! Only devices plugged straight into a controller's own ports are found
//! -- no hub support yet, so a device behind a hub (including many front
//! panels and monitors) is logged and skipped.
//!
//! A device is shared: the controller routes its events to it, and a class
//! driver (kbd.rs, storage.rs) drives it -- re-entrantly, since waiting on
//! one transfer processes events that complete others. So devices and
//! controllers live forever (leaked) and are
//! only ever reached through shared references, with everything that
//! changes in a Cell.
//!
//! The xHCI spec (eXtensible Host Controller Interface, rev 1.2) is the
//! reference for every register, bit and structure below; section numbers
//! are given where it helps.

pub mod kbd;
pub mod storage;

use alloc::boxed::Box;
use alloc::vec;
use alloc::vec::Vec;
use core::cell::Cell;
use core::sync::atomic::{Ordering, compiler_fence, fence};

use super::pci::{self, PciDevice};
use super::timer::{self, TIMER_HZ};
use crate::mm::dma;
use crate::mm::pmm::PAGE_SIZE;
use crate::mm::vmm;
use crate::mmio::Regs;
use crate::sync::Global;

/// xHCI device context index (DCI) range.
pub const MAX_ENDPOINTS: usize = 32;
const MAX_CONTROLLERS: usize = 4;
/// One page per ring; the last TRB is the link back to the start.
const RING_TRBS: u32 = (PAGE_SIZE / 16) as u32;

// Capability registers (5.3).
const CAP_CAPLENGTH: u32 = 0x00;
const CAP_HCSPARAMS1: u32 = 0x04;
const CAP_HCSPARAMS2: u32 = 0x08;
const CAP_HCCPARAMS1: u32 = 0x10;
const CAP_DBOFF: u32 = 0x14;
const CAP_RTSOFF: u32 = 0x18;

// Operational registers (5.4).
const OP_USBCMD: u32 = 0x00;
const OP_USBSTS: u32 = 0x04;
const OP_CRCR: u32 = 0x18;
const OP_DCBAAP: u32 = 0x30;
const OP_CONFIG: u32 = 0x38;
const fn op_portsc(i: u32) -> u32 {
    0x400 + 0x10 * i
}

const USBCMD_RUN: u32 = 1 << 0;
const USBCMD_HCRST: u32 = 1 << 1;
const USBSTS_HCH: u32 = 1 << 0;
const USBSTS_CNR: u32 = 1 << 11;

const PORTSC_CCS: u32 = 1 << 0;
const PORTSC_PED: u32 = 1 << 1;
const PORTSC_PR: u32 = 1 << 4;
const PORTSC_PP: u32 = 1 << 9;
const PORTSC_LWS: u32 = 1 << 16;
const PORTSC_PRC: u32 = 1 << 21;
/// CSC..CEC, all write-1-to-clear.
const PORTSC_CHANGE_BITS: u32 = 0x00FE_0000;

/// What to write back so nothing changes by accident: PED is
/// write-1-to-disable, LWS would strobe a link state change, and the
/// change bits are write-1-to-clear.
const fn portsc_keep(v: u32) -> u32 {
    v & !(PORTSC_PED | PORTSC_LWS | PORTSC_CHANGE_BITS)
}

// Interrupter 0, in the runtime registers (5.5.2).
const IR0_IMAN: u32 = 0x20;
const IR0_ERSTSZ: u32 = 0x28;
const IR0_ERSTBA: u32 = 0x30;
const IR0_ERDP: u32 = 0x38;
const ERDP_EHB: u64 = 1 << 3;

/// A Transfer Request Block (6.4).
#[repr(C)]
#[derive(Clone, Copy)]
struct Trb {
    param: u64,
    status: u32,
    control: u32,
}

const TRB_CYCLE: u32 = 1 << 0;
const TRB_TOGGLE_CYCLE: u32 = 1 << 1; // Link TRBs.
const TRB_ISP: u32 = 1 << 2;
const TRB_CHAIN: u32 = 1 << 4;
const TRB_IOC: u32 = 1 << 5;
const TRB_IDT: u32 = 1 << 6;
const TRB_DIR_IN: u32 = 1 << 16;
const fn trb_type(t: u32) -> u32 {
    t << 10
}
const fn get_type(control: u32) -> u32 {
    (control >> 10) & 0x3F
}

const TRB_NORMAL: u32 = 1;
const TRB_SETUP: u32 = 2;
const TRB_DATA: u32 = 3;
const TRB_STATUS: u32 = 4;
const TRB_LINK: u32 = 6;
const TRB_ENABLE_SLOT: u32 = 9;
const TRB_DISABLE_SLOT: u32 = 10;
const TRB_ADDRESS_DEVICE: u32 = 11;
const TRB_CONFIGURE_ENDPOINT: u32 = 12;
const TRB_EVALUATE_CONTEXT: u32 = 13;
const TRB_RESET_ENDPOINT: u32 = 14;
const TRB_SET_TR_DEQUEUE: u32 = 16;
const TRB_EVENT_TRANSFER: u32 = 32;
const TRB_EVENT_COMMAND: u32 = 33;
const TRB_EVENT_PORT_STATUS: u32 = 34;

/// Completion codes: success, a stalled endpoint, a short packet (fine).
pub const CC_SUCCESS: u32 = 1;
pub const CC_STALL: u32 = 6;
pub const CC_SHORT_PACKET: u32 = 13;

const SPEED_HIGH: u8 = 3;
const SPEED_SUPER: u8 = 4;
const SPEED_NAMES: [&str; 5] = ["?", "full", "low", "high", "super"];

const EP_TYPE_BULK_OUT: u32 = 2;
const EP_TYPE_CONTROL: u32 = 4;
const EP_TYPE_BULK_IN: u32 = 6;
const EP_TYPE_INTERRUPT_IN: u32 = 7;

#[repr(C)]
struct ErstEntry {
    base: u64,
    size: u32,
    reserved: u32,
}

/// Called when a transfer queued with queue_in() completes: the buffer it
/// was queued with, how many bytes arrived, and the completion code
/// (CC_SUCCESS and CC_SHORT_PACKET are both fine).
pub type TransferFn = fn(dev: &'static UsbDevice, buf_phys: u64, len: u32, cc: u32);

/// A TRB ring, one page.
struct Ring {
    trbs: Cell<*mut Trb>,
    phys: Cell<u64>,
    enqueue: Cell<u32>,
    cycle: Cell<u32>,
}

impl Ring {
    const fn new() -> Ring {
        Ring { trbs: Cell::new(core::ptr::null_mut()), phys: Cell::new(0), enqueue: Cell::new(0), cycle: Cell::new(0) }
    }

    fn init(&self, hc: &Xhci) -> bool {
        let Some((trbs, phys)) = hc.dma_page() else { return false };
        self.trbs.set(trbs.cast());
        self.phys.set(phys);
        self.enqueue.set(0);
        self.cycle.set(1);
        // The last slot links back to the start and flips the cycle bit.
        self.write(RING_TRBS - 1, Trb { param: phys, status: 0, control: trb_type(TRB_LINK) | TRB_TOGGLE_CYCLE });
        true
    }

    fn trb(&self, i: u32) -> *mut Trb {
        unsafe { self.trbs.get().add(i as usize) }
    }

    fn write(&self, i: u32, t: Trb) {
        unsafe { self.trb(i).write_volatile(t) };
    }

    fn contains(&self, phys: u64) -> bool {
        phys >= self.phys.get() && phys < self.phys.get() + PAGE_SIZE
    }

    /// Writes one TRB at the enqueue position and returns its physical
    /// address. The control word, with the cycle bit that hands the TRB to
    /// the controller, is written last.
    fn push(&self, param: u64, status: u32, control: u32) -> u64 {
        let i = self.enqueue.get();
        let t = self.trb(i);
        let addr = self.phys.get() + i as u64 * size_of::<Trb>() as u64;
        vwrite!(t, param, param);
        vwrite!(t, status, status);
        compiler_fence(Ordering::SeqCst);
        vwrite!(t, control, control | self.cycle.get());

        if i + 1 == RING_TRBS - 1 {
            compiler_fence(Ordering::SeqCst);
            vwrite!(self.trb(RING_TRBS - 1), control, trb_type(TRB_LINK) | TRB_TOGGLE_CYCLE | self.cycle.get());
            self.enqueue.set(0);
            self.cycle.set(self.cycle.get() ^ 1);
        } else {
            self.enqueue.set(i + 1);
        }
        addr
    }

    /// Where the controller should resume after a failed transfer: the
    /// next TRB to be written, with its cycle state.
    fn dequeue_after(&self) -> u64 {
        (self.phys.get() + self.enqueue.get() as u64 * size_of::<Trb>() as u64) | self.cycle.get() as u64
    }
}

pub struct UsbDevice {
    hc: &'static Xhci,
    pub slot: Cell<u8>,
    /// 1-based root hub port.
    pub port: u8,
    /// xHCI speed ID: 1 full, 2 low, 3 high, 4 super.
    pub speed: u8,
    pub vendor: Cell<u16>,
    pub product: Cell<u16>,
    pub dev_class: Cell<u8>,
    /// Unplugged: queue nothing more.
    pub gone: Cell<bool>,
    /// For the class driver that claimed the device.
    pub driver_data: Cell<usize>,

    // Everything below is this driver's own.
    mps0: Cell<u16>,
    /// One page each; `buf` bounces control transfers.
    in_ctx: dma::Dma,
    out_ctx: dma::Dma,
    buf: dma::Dma,
    rings: [Ring; MAX_ENDPOINTS],
    on_transfer: [Cell<Option<TransferFn>>; MAX_ENDPOINTS],
    ctrl_done: Cell<bool>,
    ctrl_cc: Cell<u32>,
    /// The one synchronous bulk transfer in flight (bulk()): its TRBs, and
    /// the byte offset and length each covers.
    bulk_done: Cell<bool>,
    bulk_cc: Cell<u32>,
    bulk_dci: Cell<u32>,
    bulk_trbs: [Cell<u64>; 4],
    bulk_offsets: [Cell<u32>; 4],
    bulk_lengths: [Cell<u32>; 4],
    bulk_trb_count: Cell<u32>,
    bulk_actual: Cell<u32>,
}

struct Xhci {
    cap: Regs,
    op: Regs,
    rt: Regs,
    doorbells: *mut u32,
    max_slots: u32,
    max_ports: u32,
    ctx_size: u32,
    /// Can DMA above 4 GiB.
    ac64: bool,
    pci: PciDevice,

    dcbaa: Cell<*mut u64>,
    cmd: Ring,
    events: Cell<*mut Trb>,
    events_phys: Cell<u64>,
    event_dequeue: Cell<u32>,
    event_cycle: Cell<u32>,

    /// The one command in flight (commands only run during enumeration).
    cmd_trb: Cell<u64>,
    cmd_done: Cell<bool>,
    cmd_cc: Cell<u32>,
    cmd_slot: Cell<u32>,

    /// By slot ID.
    devices: Vec<Cell<Option<&'static UsbDevice>>>,
    /// By 0-based port.
    port_devices: Vec<Cell<Option<&'static UsbDevice>>>,
    /// Set by port status events, handled in poll().
    port_changed: Vec<Cell<bool>>,
}

static CONTROLLERS: Global<Vec<&'static Xhci>> = Global::new(Vec::new());

fn controllers() -> &'static [&'static Xhci] {
    unsafe { CONTROLLERS.get() }
}

/// Waits up to `ms` for (register & mask) == want.
fn wait_reg(regs: Regs, off: u32, mask: u32, want: u32, ms: u32) -> bool {
    for waited in 0.. {
        if regs.rd32(off) & mask == want {
            return true;
        }
        if waited >= ms {
            return false;
        }
        timer::sleep_ms(1);
    }
    unreachable!()
}

impl Xhci {
    /// A zeroed page the controller can reach -- below 4 GiB if it can't
    /// address more: (pointer, physical).
    fn dma_page(&self) -> Option<(*mut u8, u64)> {
        let d = dma::alloc(1)?;
        if !self.ac64 && d.phys + PAGE_SIZE > 1 << 32 {
            return None;
        }
        Some((d.virt, d.phys))
    }

    fn ring_doorbell(&self, slot: u32, target: u32) {
        fence(Ordering::SeqCst);
        unsafe { self.doorbells.add(slot as usize).write_volatile(target) };
    }

    // --- Events ---

    fn handle_event(&self, ev: Trb) {
        let kind = get_type(ev.control);
        let cc = ev.status >> 24;

        if kind == TRB_EVENT_COMMAND {
            if ev.param == self.cmd_trb.get() {
                self.cmd_cc.set(cc);
                self.cmd_slot.set(ev.control >> 24);
                self.cmd_done.set(true);
            }
            return;
        }
        if kind == TRB_EVENT_PORT_STATUS {
            // Not handled here: enumerating needs to wait for events itself.
            let port = ((ev.param >> 24) & 0xFF) as u32;
            if (1..=self.max_ports).contains(&port) {
                self.port_changed[port as usize - 1].set(true);
            }
            return;
        }
        if kind != TRB_EVENT_TRANSFER {
            return;
        }

        let Some(dev) = self.devices[(ev.control >> 24) as usize].get() else { return };
        let dci = (ev.control >> 16) & 0x1F;
        if dci == 1 {
            dev.ctrl_cc.set(cc);
            dev.ctrl_done.set(true);
            return;
        }
        if dci == dev.bulk_dci.get() && !dev.bulk_done.get() {
            // Whichever of the transfer's TRBs reported (a short packet ends
            // it early), everything before that TRB moved in full.
            let residual = ev.status & 0xFF_FFFF;
            for i in 0..dev.bulk_trb_count.get() as usize {
                if ev.param == dev.bulk_trbs[i].get() {
                    let len = dev.bulk_lengths[i].get();
                    let moved = len.saturating_sub(residual);
                    dev.bulk_actual.set(dev.bulk_offsets[i].get() + moved);
                }
            }
            dev.bulk_cc.set(cc);
            dev.bulk_done.set(true);
            return;
        }
        let ring = &dev.rings[dci as usize];
        if let Some(f) = dev.on_transfer[dci as usize].get()
            && ring.contains(ev.param)
        {
            // The event names the TRB; the TRB holds the buffer and length.
            let t =
                unsafe { ring.trb(((ev.param - ring.phys.get()) / size_of::<Trb>() as u64) as u32).read_volatile() };
            let asked = t.status & 0x1FFFF;
            let residual = ev.status & 0xFF_FFFF;
            f(dev, t.param, asked.saturating_sub(residual), cc);
        }
    }

    fn process_events(&self) {
        let mut any = false;
        loop {
            let i = self.event_dequeue.get();
            let ev = unsafe { self.events.get().add(i as usize).read_volatile() };
            if ev.control & TRB_CYCLE != self.event_cycle.get() {
                break;
            }
            if i + 1 == RING_TRBS {
                self.event_dequeue.set(0);
                self.event_cycle.set(self.event_cycle.get() ^ 1);
            } else {
                self.event_dequeue.set(i + 1);
            }
            any = true;
            self.handle_event(ev);
        }
        if any {
            let dequeue = self.events_phys.get() + self.event_dequeue.get() as u64 * size_of::<Trb>() as u64;
            self.rt.wr64(IR0_ERDP, dequeue | ERDP_EHB);
        }
    }

    /// Processes events until `flag` is set: spins briefly first (most
    /// things finish in microseconds), then checks once a tick.
    fn wait_flag(&self, flag: &Cell<bool>, timeout_ms: u32) -> bool {
        for _ in 0..20000 {
            self.process_events();
            if flag.get() {
                return true;
            }
            core::hint::spin_loop();
        }
        let tick = (1000 / TIMER_HZ) as u32;
        for _ in (0..timeout_ms).step_by(tick as usize) {
            timer::sleep_ms(tick);
            self.process_events();
            if flag.get() {
                return true;
            }
        }
        false
    }

    /// Runs one command; its completion code (0 on timeout).
    fn run_command(&self, param: u64, control: u32) -> u32 {
        self.cmd_done.set(false);
        self.cmd_trb.set(self.cmd.push(param, 0, control));
        self.ring_doorbell(0, 0);
        if !self.wait_flag(&self.cmd_done, 1000) {
            kprintln!("xhci: command {} timed out", get_type(control));
            return 0;
        }
        self.cmd_cc.get()
    }
}

// --- Contexts (6.2). Input contexts have the input control context first,
// so everything sits one context further in than in the output (device)
// context. ---

impl UsbDevice {
    /// Dword `i` of input context `n` (0: input control, 1: slot, 2+:
    /// endpoints by DCI + 1).
    fn in_ctx(&self, n: u32, i: usize) -> *mut u32 {
        unsafe { self.in_ctx.virt.add((n * self.hc.ctx_size) as usize).cast::<u32>().add(i) }
    }

    fn in_get(&self, n: u32, i: usize) -> u32 {
        unsafe { self.in_ctx(n, i).read_volatile() }
    }

    fn in_set(&self, n: u32, i: usize, v: u32) {
        unsafe { self.in_ctx(n, i).write_volatile(v) }
    }

    /// Points endpoint context `ep` (an input context index) at its ring;
    /// the dequeue cycle state starts at 1.
    fn set_ep_dequeue(&self, ep: u32, dci: usize) {
        let dq = self.rings[dci].phys.get() | 1;
        self.in_set(ep, 2, dq as u32);
        self.in_set(ep, 3, (dq >> 32) as u32);
    }

    fn clear_in_ctx(&self) {
        unsafe { core::ptr::write_bytes(self.in_ctx.virt, 0, PAGE_SIZE as usize) };
    }

    /// Copies the slot context from the output context and widens its
    /// "context entries" to cover `dci`, for adding an endpoint.
    fn slot_ctx_covering(&self, dci: u32) {
        unsafe {
            core::ptr::copy_nonoverlapping(self.out_ctx.virt, self.in_ctx(1, 0).cast(), self.hc.ctx_size as usize)
        };
        let s0 = self.in_get(1, 0);
        if dci > (s0 >> 27) & 0x1F {
            self.in_set(1, 0, s0 & !(0x1F << 27) | dci << 27);
        }
    }

    fn configure_endpoint(&self) -> u32 {
        self.hc.run_command(self.in_ctx.phys, trb_type(TRB_CONFIGURE_ENDPOINT) | (self.slot.get() as u32) << 24)
    }

    /// After a STALL the endpoint is halted until reset, and its dequeue
    /// pointer has to be moved past the failed transfer (4.6.8).
    fn recover_endpoint(&self, dci: u32) {
        let target = dci << 16 | (self.slot.get() as u32) << 24;
        self.hc.run_command(0, trb_type(TRB_RESET_ENDPOINT) | target);
        self.hc.run_command(self.rings[dci as usize].dequeue_after(), trb_type(TRB_SET_TR_DEQUEUE) | target);
    }

    // --- Control transfers ---

    /// A control transfer on endpoint 0. `data` is ordinary kernel memory,
    /// up to a page: written to the device for an OUT request, filled for an
    /// IN one. False on any failure (including a STALL, after which the
    /// endpoint is reset and usable again).
    pub fn control(&self, request_type: u8, request: u8, value: u16, index: u16, data: &mut [u8]) -> bool {
        let length = data.len();
        if length > PAGE_SIZE as usize {
            return false;
        }
        let input = request_type & 0x80 != 0;
        let buf = unsafe { core::slice::from_raw_parts_mut(self.buf.virt, length) };
        if !input {
            buf.copy_from_slice(data);
        }

        let r = &self.rings[1];
        let setup = request_type as u64
            | (request as u64) << 8
            | (value as u64) << 16
            | (index as u64) << 32
            | (length as u64) << 48;
        let trt = if length == 0 {
            0
        } else if input {
            3
        } else {
            2
        }; // Transfer type: none / IN / OUT data.
        r.push(setup, 8, trb_type(TRB_SETUP) | TRB_IDT | trt << 16);
        if length > 0 {
            r.push(self.buf.phys, length as u32, trb_type(TRB_DATA) | if input { TRB_DIR_IN } else { 0 });
        }
        // The status stage runs opposite to the data stage (IN if none).
        let status_dir = if length == 0 || !input { TRB_DIR_IN } else { 0 };
        r.push(0, 0, trb_type(TRB_STATUS) | status_dir | TRB_IOC);

        self.ctrl_done.set(false);
        self.hc.ring_doorbell(self.slot.get() as u32, 1);
        if !self.hc.wait_flag(&self.ctrl_done, 1000) {
            kprintln!("usb: port {}: control request 0x{request:x} timed out", self.port);
            return false;
        }
        let cc = self.ctrl_cc.get();
        if cc != CC_SUCCESS && cc != CC_SHORT_PACKET {
            if cc == CC_STALL {
                self.recover_endpoint(1);
            }
            return false;
        }
        if input {
            data.copy_from_slice(buf);
        }
        true
    }

    /// A control request with no data stage.
    pub fn control_nodata(&self, request_type: u8, request: u8, value: u16, index: u16) -> bool {
        self.control(request_type, request, value, index, &mut [])
    }

    // --- Interrupt and bulk endpoints ---

    /// Configures interrupt IN endpoint `ep_address` (from its endpoint
    /// descriptor) and sets `f` to receive its completions. The endpoint's
    /// DCI for queue_in().
    pub fn add_interrupt_in(&self, ep_address: u8, max_packet: u16, interval: u8, f: TransferFn) -> Option<u32> {
        let dci = (ep_address & 0x0F) as u32 * 2 + 1;
        if !self.rings[dci as usize].init(self.hc) {
            return None;
        }
        self.clear_in_ctx();
        self.in_set(0, 1, 1 | 1 << dci); // Add: slot context + this endpoint.
        self.slot_ctx_covering(dci);

        let ep = dci + 1;
        self.in_set(ep, 0, xhci_interval(self.speed, interval) << 16);
        self.in_set(ep, 1, 3 << 1 | EP_TYPE_INTERRUPT_IN << 3 | (max_packet as u32) << 16);
        self.set_ep_dequeue(ep, dci as usize);
        self.in_set(ep, 4, max_packet as u32 | (max_packet as u32) << 16); // Average TRB length, max ESIT payload.

        let cc = self.configure_endpoint();
        if cc != CC_SUCCESS {
            kprintln!("usb: port {}: configuring endpoint 0x{ep_address:x} failed ({cc})", self.port);
            return None;
        }
        self.on_transfer[dci as usize].set(Some(f));
        Some(dci)
    }

    /// Configures bulk endpoint `ep_address` (IN or OUT) for bulk(). Its DCI.
    pub fn add_bulk(&self, ep_address: u8, max_packet: u16) -> Option<u32> {
        let input = ep_address & 0x80 != 0;
        let dci = (ep_address & 0x0F) as u32 * 2 + input as u32;
        if !self.rings[dci as usize].init(self.hc) {
            return None;
        }
        self.clear_in_ctx();
        self.in_set(0, 1, 1 | 1 << dci);
        self.slot_ctx_covering(dci);

        let ep = dci + 1;
        let kind = if input { EP_TYPE_BULK_IN } else { EP_TYPE_BULK_OUT };
        self.in_set(ep, 1, 3 << 1 | kind << 3 | (max_packet as u32) << 16);
        self.set_ep_dequeue(ep, dci as usize);
        self.in_set(ep, 4, 3072); // Average TRB length: a guess, only used for scheduling.

        let cc = self.configure_endpoint();
        if cc != CC_SUCCESS {
            kprintln!("usb: port {}: configuring bulk endpoint 0x{ep_address:x} failed ({cc})", self.port);
            return None;
        }
        Some(dci)
    }

    /// One bulk transfer of up to 64 KiB to or from `buf_phys` (DMA-able,
    /// e.g. from the PMM), waiting for it to finish: (completion code,
    /// bytes moved). The code is CC_SUCCESS, CC_SHORT_PACKET, CC_STALL (the
    /// endpoint stalled and has been reset on the host side), or 0 on
    /// timeout.
    pub fn bulk(&self, dci: u32, buf_phys: u64, length: u32, timeout_ms: u32) -> (u32, u32) {
        if self.gone.get() || length > 0x10000 {
            return (0, 0);
        }
        // A TRB's buffer can't cross a 64 KiB boundary (6.4.1), so the
        // transfer is a chain of TRBs split there.
        let r = &self.rings[dci as usize];
        self.bulk_trb_count.set(0);
        self.bulk_actual.set(0);
        let mut done = 0;
        loop {
            let at = buf_phys + done as u64;
            let to_boundary = 0x10000 - (at & 0xFFFF) as u32;
            let n = (length - done).min(to_boundary);
            let last = done + n == length;
            let i = self.bulk_trb_count.get() as usize;
            self.bulk_trb_count.set(i as u32 + 1);
            self.bulk_offsets[i].set(done);
            self.bulk_lengths[i].set(n);
            let flags = trb_type(TRB_NORMAL) | TRB_ISP | if last { TRB_IOC } else { TRB_CHAIN };
            self.bulk_trbs[i].set(r.push(at, n, flags));
            done += n;
            if done >= length {
                break;
            }
        }

        self.bulk_dci.set(dci);
        self.bulk_done.set(false);
        self.hc.ring_doorbell(self.slot.get() as u32, dci);
        let finished = self.hc.wait_flag(&self.bulk_done, timeout_ms);
        self.bulk_dci.set(0);
        if !finished {
            return (0, 0);
        }
        let cc = self.bulk_cc.get();
        if cc == CC_SUCCESS {
            self.bulk_actual.set(length);
        }
        if cc == CC_STALL {
            self.recover_endpoint(dci);
        }
        (cc, self.bulk_actual.get())
    }

    /// Queues one IN transfer into `buf_phys` (DMA-able memory, e.g. from
    /// the PMM). It completes through the endpoint's TransferFn.
    pub fn queue_in(&self, dci: u32, buf_phys: u64, length: u16) {
        self.rings[dci as usize].push(buf_phys, length as u32, trb_type(TRB_NORMAL) | TRB_ISP | TRB_IOC);
        self.hc.ring_doorbell(self.slot.get() as u32, dci);
    }
}

/// xHCI wants the polling interval as 2^n units of 125 us; full/low speed
/// descriptors give it in 1 ms frames, high/super speed already as an
/// exponent (plus one).
fn xhci_interval(speed: u8, interval: u8) -> u32 {
    if speed == SPEED_HIGH || speed == SPEED_SUPER {
        return (interval.max(1) as u32 - 1).min(15);
    }
    let frames_125us = interval.max(1) as u32 * 8;
    let mut n = 0;
    while 2 << n <= frames_125us {
        n += 1;
    }
    n.clamp(3, 10)
}

// --- Enumeration ---

impl Xhci {
    /// Resets a USB 2 port, which is what enables it; a USB 3 port enables
    /// itself during link training, so it's already enabled by now.
    fn port_reset(&self, i: u32) -> bool {
        let sc = self.op.rd32(op_portsc(i));
        if sc & PORTSC_PED != 0 {
            return true;
        }
        self.op.wr32(op_portsc(i), portsc_keep(sc) | PORTSC_PR);
        if !wait_reg(self.op, op_portsc(i), PORTSC_PRC, PORTSC_PRC, 500) {
            return false;
        }
        let sc = self.op.rd32(op_portsc(i));
        self.op.wr32(op_portsc(i), portsc_keep(sc) | PORTSC_PRC);
        timer::sleep_ms(20); // Reset recovery (USB 2.0 7.1.7.5: 10 ms minimum).
        self.op.rd32(op_portsc(i)) & PORTSC_PED != 0
    }

    fn new_device(&'static self, port_index: u32, speed: u8) -> Option<&'static UsbDevice> {
        let page = || self.dma_page().map(|(virt, phys)| dma::Dma { virt, phys });
        let dev: &'static UsbDevice = Box::leak(Box::new(UsbDevice {
            hc: self,
            slot: Cell::new(0),
            port: port_index as u8 + 1,
            speed,
            vendor: Cell::new(0),
            product: Cell::new(0),
            dev_class: Cell::new(0),
            gone: Cell::new(false),
            driver_data: Cell::new(0),
            mps0: Cell::new(match speed {
                SPEED_HIGH => 64,
                SPEED_SUPER => 512,
                _ => 8, // Low speed always; full speed until the descriptor says.
            }),
            in_ctx: page()?,
            out_ctx: page()?,
            buf: page()?,
            rings: [const { Ring::new() }; MAX_ENDPOINTS],
            on_transfer: [const { Cell::new(None) }; MAX_ENDPOINTS],
            ctrl_done: Cell::new(false),
            ctrl_cc: Cell::new(0),
            bulk_done: Cell::new(false),
            bulk_cc: Cell::new(0),
            bulk_dci: Cell::new(0),
            bulk_trbs: [const { Cell::new(0) }; 4],
            bulk_offsets: [const { Cell::new(0) }; 4],
            bulk_lengths: [const { Cell::new(0) }; 4],
            bulk_trb_count: Cell::new(0),
            bulk_actual: Cell::new(0),
        }));
        if !dev.rings[1].init(self) {
            return None;
        }
        Some(dev)
    }

    fn address_device(&self, dev: &'static UsbDevice) -> bool {
        let cc = self.run_command(0, trb_type(TRB_ENABLE_SLOT));
        if cc != CC_SUCCESS {
            kprintln!("usb: port {}: no free device slot ({cc})", dev.port);
            return false;
        }
        let slot = self.cmd_slot.get() as u8;
        dev.slot.set(slot);
        self.devices[slot as usize].set(Some(dev));
        unsafe { self.dcbaa.get().add(slot as usize).write_volatile(dev.out_ctx.phys) };

        dev.in_set(0, 1, 0x3); // Add: slot context + endpoint 0.
        dev.in_set(1, 0, (dev.speed as u32) << 20 | 1 << 27); // One context entry.
        dev.in_set(1, 1, (dev.port as u32) << 16); // Root hub port.
        dev.in_set(2, 1, 3 << 1 | EP_TYPE_CONTROL << 3 | (dev.mps0.get() as u32) << 16);
        dev.set_ep_dequeue(2, 1);
        dev.in_set(2, 4, 8); // Average TRB length.

        let cc = self.run_command(dev.in_ctx.phys, trb_type(TRB_ADDRESS_DEVICE) | (slot as u32) << 24);
        if cc != CC_SUCCESS {
            kprintln!("usb: port {}: Address Device failed ({cc})", dev.port);
            return false;
        }
        timer::sleep_ms(2); // SET_ADDRESS recovery (USB 2.0 9.2.6.3).
        true
    }

    /// Full-speed devices may use an endpoint 0 packet size other than the
    /// 8 bytes assumed until the first 8 bytes of the descriptor are in.
    fn fix_mps0(&self, dev: &UsbDevice, mps0: u16) -> bool {
        if mps0 == dev.mps0.get() || mps0 == 0 {
            return true;
        }
        dev.mps0.set(mps0);
        dev.clear_in_ctx();
        dev.in_set(0, 1, 1 << 1); // Evaluate endpoint 0 only.
        unsafe {
            core::ptr::copy_nonoverlapping(
                dev.out_ctx.virt.add(self.ctx_size as usize),
                dev.in_ctx(2, 0).cast(),
                self.ctx_size as usize,
            )
        };
        dev.in_set(2, 1, dev.in_get(2, 1) & 0x0000_FFFF | (mps0 as u32) << 16);
        self.run_command(dev.in_ctx.phys, trb_type(TRB_EVALUATE_CONTEXT) | (dev.slot.get() as u32) << 24) == CC_SUCCESS
    }

    fn enumerate_port(&'static self, i: u32) {
        if !self.port_reset(i) {
            kprintln!("usb: port {}: reset failed", i + 1);
            return;
        }
        let speed = ((self.op.rd32(op_portsc(i)) >> 10) & 0xF) as u8;
        let Some(dev) = self.new_device(i, speed) else {
            kprintln!("usb: port {}: out of memory", i + 1);
            return;
        };
        self.port_devices[i as usize].set(Some(dev));
        if !self.address_device(dev) {
            return;
        }

        let mut desc = [0u8; 18];
        if !dev.control(0x80, 6, 0x0100, 0, &mut desc[..8]) {
            // GET_DESCRIPTOR(device), 8 bytes
            kprintln!("usb: port {}: no device descriptor", dev.port);
            return;
        }
        let mps0 = if speed == SPEED_SUPER { 1 << desc[7] } else { desc[7] as u16 };
        if !self.fix_mps0(dev, mps0) || !dev.control(0x80, 6, 0x0100, 0, &mut desc) {
            kprintln!("usb: port {}: device descriptor failed", dev.port);
            return;
        }
        dev.dev_class.set(desc[4]);
        dev.vendor.set(u16::from_le_bytes([desc[8], desc[9]]));
        dev.product.set(u16::from_le_bytes([desc[10], desc[11]]));
        kprintln!(
            "usb: port {}: {:x}:{:x}, {} speed, class {}",
            dev.port,
            dev.vendor.get(),
            dev.product.get(),
            SPEED_NAMES.get(speed as usize).unwrap_or(&"?"),
            desc[4]
        );
        if desc[4] == 9 {
            kprintln!(
                "usb: port {}: a USB hub -- not supported yet, so nothing behind it works; plug the keyboard straight into the PC",
                dev.port
            );
            return;
        }

        let mut config = vec![0u8; PAGE_SIZE as usize];
        if !dev.control(0x80, 6, 0x0200, 0, &mut config[..9]) {
            kprintln!("usb: port {}: no configuration descriptor", dev.port);
            return;
        }
        let total = (u16::from_le_bytes([config[2], config[3]]) as usize).min(PAGE_SIZE as usize);
        if dev.control(0x80, 6, 0x0200, 0, &mut config[..total]) && !kbd::probe(dev, &config[..total]) {
            storage::probe(dev, &config[..total]);
        }
    }

    /// Takes the controller from the firmware's "USB legacy support" -- the
    /// SMM code that emulates a PS/2 keyboard for the boot menu -- which
    /// otherwise keeps fighting the driver for it (4.22.1).
    fn bios_handoff(&self) {
        let mut off = ((self.cap.rd32(CAP_HCCPARAMS1) >> 16) & 0xFFFF) * 4;
        while off != 0 {
            let v = self.cap.rd32(off);
            if v & 0xFF == 1 {
                if v & 1 << 16 != 0 {
                    self.cap.wr8(off + 3, 1); // OS-owned semaphore.
                    if !wait_reg(self.cap, off, 1 << 16, 0, 1000) {
                        kprintln!("xhci: firmware won't release the controller -- taking it anyway");
                        self.cap.wr8(off + 2, 0);
                    }
                }
                // Disable the firmware's SMIs, clearing any it left pending.
                self.cap.wr32(off + 4, self.cap.rd32(off + 4) & 0xFFFF_1FEE | 0xE000_0000);
                return;
            }
            let next = (v >> 8) & 0xFF;
            off = if next != 0 { off + next * 4 } else { 0 };
        }
    }

    /// Resets and starts the controller: device context array, scratchpad,
    /// command ring, event ring, then the ports.
    fn start(&self) -> bool {
        // Stop, then reset.
        self.op.wr32(OP_USBCMD, self.op.rd32(OP_USBCMD) & !USBCMD_RUN);
        if !wait_reg(self.op, OP_USBSTS, USBSTS_HCH, USBSTS_HCH, 100) {
            kprintln!("xhci: controller won't halt");
            return false;
        }
        self.op.wr32(OP_USBCMD, USBCMD_HCRST);
        timer::sleep_ms(1);
        if !wait_reg(self.op, OP_USBCMD, USBCMD_HCRST, 0, 1000) || !wait_reg(self.op, OP_USBSTS, USBSTS_CNR, 0, 1000) {
            kprintln!("xhci: controller reset timed out");
            return false;
        }

        self.op.wr32(OP_CONFIG, self.max_slots);
        let Some((dcbaa, dcbaa_phys)) = self.dma_page() else { return false };
        self.dcbaa.set(dcbaa.cast());
        // Scratchpad: pages the controller keeps for itself (4.20).
        let hcs2 = self.cap.rd32(CAP_HCSPARAMS2);
        let scratch = ((hcs2 >> 27) & 0x1F) | ((hcs2 >> 21) & 0x1F) << 5;
        if scratch > 0 {
            let Some((array, array_phys)) = self.dma_page() else { return false };
            for i in 0..scratch as usize {
                let Some((_, phys)) = self.dma_page() else { return false };
                unsafe { array.cast::<u64>().add(i).write_volatile(phys) };
            }
            unsafe { dcbaa.cast::<u64>().write_volatile(array_phys) };
        }
        self.op.wr64(OP_DCBAAP, dcbaa_phys);

        if !self.cmd.init(self) {
            return false;
        }
        self.op.wr64(OP_CRCR, self.cmd.phys.get() | 1);

        let (Some((events, events_phys)), Some((erst, erst_phys))) = (self.dma_page(), self.dma_page()) else {
            return false;
        };
        self.events.set(events.cast());
        self.events_phys.set(events_phys);
        unsafe {
            erst.cast::<ErstEntry>().write_volatile(ErstEntry { base: events_phys, size: RING_TRBS, reserved: 0 })
        };
        self.event_dequeue.set(0);
        self.event_cycle.set(1);
        self.rt.wr32(IR0_ERSTSZ, 1);
        self.rt.wr64(IR0_ERDP, events_phys);
        self.rt.wr64(IR0_ERSTBA, erst_phys);
        self.rt.wr32(IR0_IMAN, 1); // Clear any pending flag; interrupts stay off.

        self.op.wr32(OP_USBCMD, USBCMD_RUN);
        if !wait_reg(self.op, OP_USBSTS, USBSTS_HCH, 0, 100) {
            kprintln!("xhci: controller won't start");
            return false;
        }

        // Power the ports if that's software's job (HCCPARAMS1.PPC), then
        // give devices time to connect and USB 3 links time to train.
        if self.cap.rd32(CAP_HCCPARAMS1) & 1 << 3 != 0 {
            for i in 0..self.max_ports {
                let sc = self.op.rd32(op_portsc(i));
                if sc & PORTSC_PP == 0 {
                    self.op.wr32(op_portsc(i), portsc_keep(sc) | PORTSC_PP);
                }
            }
        }
        timer::sleep_ms(300);
        true
    }

    /// A device was unplugged: stop routing its events and free its slot.
    /// Its memory is leaked -- small, and only on unplug.
    fn release_port(&self, i: u32) {
        let Some(dev) = self.port_devices[i as usize].replace(None) else { return };
        dev.gone.set(true);
        let slot = dev.slot.get();
        if slot != 0 {
            self.devices[slot as usize].set(None);
            self.run_command(0, trb_type(TRB_DISABLE_SLOT) | (slot as u32) << 24);
            unsafe { self.dcbaa.get().add(slot as usize).write_volatile(0) };
        }
        kprintln!("usb: port {}: unplugged", i + 1);
    }

    /// Clears the port's change bits -- a new status change event only
    /// comes once they're clear -- then enumerates or releases it to match.
    fn port_changed(&'static self, i: u32) {
        let sc = self.op.rd32(op_portsc(i));
        self.op.wr32(op_portsc(i), portsc_keep(sc) | sc & PORTSC_CHANGE_BITS);
        let connected = sc & PORTSC_CCS != 0;
        let present = self.port_devices[i as usize].get().is_some();
        if !connected && present {
            self.release_port(i);
        } else if connected && !present {
            timer::sleep_ms(100); // Connect debounce (USB 2.0 7.1.7.3).
            self.enumerate_port(i);
        }
    }
}

/// Maps a controller's registers and reads its capabilities.
fn probe_controller(pci: &PciDevice) -> Xhci {
    // Memory decoding + bus mastering on, legacy INTx off (we poll).
    pci.write16(0x04, pci.read16(0x04) | 0x0406);
    let mut bar = (pci.bar[0] & !0xF) as u64;
    if pci.bar[0] & 0x6 == 0x4 {
        bar |= (pci.bar[1] as u64) << 32;
    }
    let probe = Regs(vmm::map_mmio(bar, 0x1000));
    let caplen = probe.rd8(CAP_CAPLENGTH) as u32;
    let hcs1 = probe.rd32(CAP_HCSPARAMS1);
    let dboff = probe.rd32(CAP_DBOFF) & !0x3;
    let rtsoff = probe.rd32(CAP_RTSOFF) & !0x1F;
    let max_slots = hcs1 & 0xFF;
    let max_ports = (hcs1 >> 24) & 0xFF;

    let size = (caplen + op_portsc(max_ports)).max(rtsoff + 0x40).max(dboff + (max_slots + 1) * 4);
    let cap = Regs(vmm::map_mmio(bar, size as u64));
    let hcc1 = cap.rd32(CAP_HCCPARAMS1);
    Xhci {
        cap,
        op: cap.at(caplen),
        rt: cap.at(rtsoff),
        doorbells: cap.at(dboff).0.cast(),
        max_slots,
        max_ports,
        ctx_size: if hcc1 & 1 << 2 != 0 { 64 } else { 32 },
        ac64: hcc1 & 1 != 0,
        pci: *pci,
        dcbaa: Cell::new(core::ptr::null_mut()),
        cmd: Ring::new(),
        events: Cell::new(core::ptr::null_mut()),
        events_phys: Cell::new(0),
        event_dequeue: Cell::new(0),
        event_cycle: Cell::new(0),
        cmd_trb: Cell::new(0),
        cmd_done: Cell::new(false),
        cmd_cc: Cell::new(0),
        cmd_slot: Cell::new(0),
        devices: (0..256).map(|_| Cell::new(None)).collect(),
        port_devices: (0..256).map(|_| Cell::new(None)).collect(),
        port_changed: (0..256).map(|_| Cell::new(false)).collect(),
    }
}

/// Finds every xHCI controller (PCI class 0C.03, prog-if 30), takes it
/// over from the firmware, resets it, and enumerates whatever is plugged
/// into its ports, offering each device to the class drivers. The timer
/// must be up. How many controllers started.
pub fn init() -> i32 {
    let list = unsafe { CONTROLLERS.get() };
    for n in 0.. {
        let Some(pci) = pci::find_class_nth(0x0C, 0x03, 0x30, n) else { break };
        if list.len() == MAX_CONTROLLERS {
            kprintln!("xhci: more than {MAX_CONTROLLERS} controllers -- ignoring the rest");
            break;
        }
        kprintln!(
            "xhci: controller at {}:{}.{} ({:x}:{:x})",
            pci.bus,
            pci.slot,
            pci.func,
            pci.vendor_id,
            pci.device_id
        );
        let hc = probe_controller(pci);
        hc.bios_handoff();
        let hc: &'static Xhci = Box::leak(Box::new(hc));
        if !hc.start() {
            continue; // Leaked: only on a controller that failed to start.
        }
        list.push(hc);

        let mut connected = 0;
        for i in 0..hc.max_ports {
            let sc = hc.op.rd32(op_portsc(i));
            hc.op.wr32(op_portsc(i), portsc_keep(sc) | sc & PORTSC_CHANGE_BITS);
            if sc & PORTSC_CCS != 0 {
                connected += 1;
                hc.enumerate_port(i);
            }
        }
        hc.process_events();
        hc.port_changed.iter().for_each(|c| c.set(false)); // Just handled them all.
        kprintln!(
            "xhci: {}:{}.{}: {} ports, {connected} connected",
            hc.pci.bus,
            hc.pci.slot,
            hc.pci.func,
            hc.max_ports
        );
    }
    list.len() as i32
}

/// Processes any pending controller events -- completed transfers land in
/// their TransferFn here -- and handles plugs and unplugs. Cheap when
/// nothing happened.
pub fn poll() {
    for &hc in controllers() {
        hc.process_events();
        for i in 0..hc.max_ports {
            if hc.port_changed[i as usize].replace(false) {
                hc.port_changed(i);
            }
        }
    }
}
