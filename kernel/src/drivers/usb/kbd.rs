//! USB keyboards, through the HID boot protocol: the fixed 8-byte report
//! every USB keyboard supports for BIOS setup screens (modifier bits plus
//! up to six pressed keys), so there's no HID report descriptor to parse.
//! Key repeat is done here, from the timer, since the boot protocol only
//! reports what's held.

use super::{CC_SHORT_PACKET, CC_SUCCESS, UsbDevice};
use crate::boot::requests::phys_to_virt;
use crate::drivers::{keymap, timer};
use crate::mm::dma;
use crate::sync::Global;

const MAX_KEYBOARDS: usize = 4;
/// Transfers kept in flight per keyboard.
const REPORTS_QUEUED: u64 = 8;
/// Bytes between the report buffers in their page.
const REPORT_STRIDE: u64 = 64;
const REPEAT_DELAY_MS: u64 = 500;
/// About 30 a second.
const REPEAT_RATE_MS: u64 = 33;

const HID_SET_IDLE: u8 = 0x0A;
const HID_SET_PROTOCOL: u8 = 0x0B;
const HID_USAGE_CAPS_LOCK: u8 = 57;
/// "Too many keys held" -- the report means nothing.
const HID_ERROR_ROLLOVER: u8 = 1;

const MOD_CTRL: u8 = 0x11; // Left | right.
const MOD_SHIFT: u8 = 0x22;

#[derive(Clone, Copy)]
struct Keyboard {
    /// None: a free slot.
    dev: Option<&'static UsbDevice>,
    dci: u32,
    /// Previous report: what's held now.
    last: [u8; 8],
}

struct State {
    keyboards: [Keyboard; MAX_KEYBOARDS],
    caps_lock: bool,
    /// Repeat: the most recently pressed key that's still held (usage 0:
    /// none), on which keyboard, with which modifiers, and when next.
    repeat_kb: usize,
    repeat_usage: u8,
    repeat_mods: u8,
    repeat_at_ms: u64,
    /// Bytes typed but not yet read.
    queue: [u8; 64],
    head: usize,
    tail: usize,
}

static STATE: Global<State> = Global::new(State {
    keyboards: [Keyboard { dev: None, dci: 0, last: [0; 8] }; MAX_KEYBOARDS],
    caps_lock: false,
    repeat_kb: usize::MAX,
    repeat_usage: 0,
    repeat_mods: 0,
    repeat_at_ms: 0,
    queue: [0; 64],
    head: 0,
    tail: 0,
});

fn state() -> &'static mut State {
    unsafe { STATE.get() }
}

/// HID usage IDs (the USB HID Usage Tables, keyboard page) -> Linux key
/// codes, which is what keymap.rs speaks. Same table as Linux's own
/// usbkbd.c; past 100 is nothing a terminal has a byte for.
const HID_TO_KEYCODE: [u8; 101] = [
    0, 0, 0, 0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 28, 1, 14, 15, 57, 12, 13, 26, 27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61,
    62, 63, 64, 65, 66, 67, 68, 87, 88, 99, 70, 119, 110, 102, 104, 111, 107, 109, 106, 105, 108, 103, 69, 98, 55, 74,
    78, 96, 79, 80, 81, 75, 76, 77, 71, 72, 73, 82, 83, 86,
];

impl State {
    fn push_bytes(&mut self, bytes: &[u8]) {
        for &b in bytes {
            let next = (self.head + 1) % self.queue.len();
            if next == self.tail {
                return; // Full -- nobody's reading; drop the rest.
            }
            self.queue[self.head] = b;
            self.head = next;
        }
    }

    fn type_key(&mut self, usage: u8, mods: u8) {
        let Some(&code) = HID_TO_KEYCODE.get(usage as usize) else { return };
        let m = if mods & MOD_SHIFT != 0 { keymap::MOD_SHIFT } else { 0 }
            | if mods & MOD_CTRL != 0 { keymap::MOD_CTRL } else { 0 }
            | if self.caps_lock { keymap::MOD_CAPS } else { 0 };
        if let Some(bytes) = keymap::bytes(code as u16, m) {
            self.push_bytes(bytes);
        }
    }

    fn handle_report(&mut self, kb: usize, r: &[u8; 8]) {
        if r[2] == HID_ERROR_ROLLOVER {
            return;
        }
        let held = |report: &[u8; 8], usage: u8| report[2..].contains(&usage);
        for &usage in &r[2..] {
            if usage == 0 || held(&self.keyboards[kb].last, usage) {
                continue; // Nothing, or still held from before.
            }
            if usage == HID_USAGE_CAPS_LOCK {
                self.caps_lock = !self.caps_lock;
                continue;
            }
            self.type_key(usage, r[0]);
            self.repeat_kb = kb;
            self.repeat_usage = usage;
            self.repeat_at_ms = timer::uptime_ms() + REPEAT_DELAY_MS;
        }
        if self.repeat_kb == kb {
            if self.repeat_usage != 0 && !held(r, self.repeat_usage) {
                self.repeat_usage = 0;
            }
            self.repeat_mods = r[0];
        }
        self.keyboards[kb].last = *r;
    }
}

fn on_report(dev: &'static UsbDevice, buf_phys: u64, len: u32, cc: u32) {
    let kb = dev.driver_data.get();
    let s = state();
    if cc != CC_SUCCESS && cc != CC_SHORT_PACKET {
        // Most likely unplugged; anything else would need the endpoint
        // reset, and a keyboard that stopped is better than a loop of
        // failing transfers.
        kprintln!("usb-kbd: port {}: transfer failed ({cc})", dev.port);
        if s.repeat_kb == kb {
            s.repeat_usage = 0;
        }
        return;
    }
    if len >= 3 {
        let mut report = [0u8; 8];
        let n = (len as usize).min(8);
        unsafe { core::ptr::copy_nonoverlapping(phys_to_virt::<u8>(buf_phys), report.as_mut_ptr(), n) };
        s.handle_report(kb, &report);
    }
    if !dev.gone.get() {
        dev.queue_in(s.keyboards[kb].dci, buf_phys, 8); // Reuse the buffer for the next report.
    }
}

/// Called for each newly enumerated device with its whole configuration
/// descriptor. Claims it if it has a boot keyboard interface; true if it
/// did.
pub fn probe(dev: &'static UsbDevice, config: &[u8]) -> bool {
    let s = state();
    // A slot never used, or whose keyboard was unplugged.
    let Some(kb) = s.keyboards.iter().position(|k| k.dev.is_none_or(|d| d.gone.get())) else { return false };

    // A HID boot keyboard interface (class 3, subclass 1, protocol 1) and
    // its interrupt IN endpoint.
    let mut in_keyboard = false;
    let (mut interface, mut ep_address, mut interval, mut max_packet) = (0u8, 0u8, 0u8, 0u16);
    let mut off = 0;
    while off + 2 <= config.len() && config[off] >= 2 {
        let d = &config[off..];
        if off + d[0] as usize > config.len() {
            break;
        }
        if d[1] == 4 && d[0] >= 9 {
            // Interface
            in_keyboard = d[5] == 3 && d[6] == 1 && d[7] == 1;
            interface = d[2];
        } else if d[1] == 5 && d[0] >= 7 && in_keyboard && d[2] & 0x80 != 0 && d[3] & 0x3 == 3 {
            // Interrupt IN endpoint
            ep_address = d[2];
            max_packet = u16::from_le_bytes([d[4], d[5]]) & 0x7FF;
            interval = d[6];
            break;
        }
        off += d[0] as usize;
    }
    if ep_address == 0 {
        return false;
    }

    if s.repeat_kb == kb {
        s.repeat_kb = usize::MAX;
        s.repeat_usage = 0;
    }
    s.keyboards[kb] = Keyboard { dev: None, dci: 0, last: [0; 8] };
    dev.driver_data.set(kb);

    if !dev.control_nodata(0x00, 9, config[5] as u16, 0) {
        // SET_CONFIGURATION
        kprintln!("usb-kbd: port {}: SET_CONFIGURATION failed", dev.port);
        return false;
    }
    if !dev.control_nodata(0x21, HID_SET_PROTOCOL, 0, interface as u16) {
        kprintln!("usb-kbd: port {}: can't switch to the boot protocol", dev.port);
        return false;
    }
    // Report only on change; repeat is ours. Optional -- some keyboards
    // refuse it, which is fine.
    dev.control_nodata(0x21, HID_SET_IDLE, 0, interface as u16);

    let Some(dci) = dev.add_interrupt_in(ep_address, max_packet, interval, on_report) else { return false };
    // One page holding REPORTS_QUEUED buffers REPORT_STRIDE apart.
    let Some(reports) = dma::alloc(1) else { return false };
    let s = state();
    s.keyboards[kb].dci = dci;
    for i in 0..REPORTS_QUEUED {
        dev.queue_in(dci, reports.phys + i * REPORT_STRIDE, 8);
    }
    s.keyboards[kb].dev = Some(dev);
    kprintln!("usb-kbd: port {}: keyboard ready", dev.port);
    true
}

/// How many USB keyboards are plugged in and working.
pub fn count() -> usize {
    state().keyboards.iter().filter(|k| k.dev.is_some_and(|d| !d.gone.get())).count()
}

/// Non-blocking: the next byte typed on any USB keyboard (polling the
/// controllers as it goes), or None. Same bytes as every other keyboard --
/// see keymap.rs.
pub fn poll_char() -> Option<u8> {
    super::poll(); // Cheap with nothing pending; also notices hot-plugged keyboards.

    let s = state();
    if s.repeat_usage != 0 && s.keyboards[s.repeat_kb].dev.is_none_or(|d| d.gone.get()) {
        s.repeat_usage = 0; // Unplugged with the key down.
    }
    if s.repeat_usage != 0 && s.head == s.tail {
        let now = timer::uptime_ms();
        if now >= s.repeat_at_ms {
            s.type_key(s.repeat_usage, s.repeat_mods);
            s.repeat_at_ms = now + REPEAT_RATE_MS;
        }
    }
    if s.head == s.tail {
        return None;
    }
    let c = s.queue[s.tail];
    s.tail = (s.tail + 1) % s.queue.len();
    Some(c)
}
