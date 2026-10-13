//! virtio-input keyboard. QEMU exposes keyboard, mouse and tablet under
//! the same vendor:device PCI id, so this walks every match and picks the
//! one whose VIRTIO_INPUT_CFG_ID_NAME config string contains "keyboard".

use super::{Buffer, VirtioDevice, Virtqueue};
use crate::boot::requests::phys_to_virt;
use crate::drivers::{keymap, pci};
use crate::mm::pmm::{self, PAGE_SIZE};
use crate::sync::Global;

const PCI_VENDOR_ID: u16 = 0x1af4;
const PCI_DEVICE_ID: u16 = 0x1052;

const CFG_ID_NAME: u8 = 0x01;
const EV_KEY: u16 = 1;

const KEY_LEFTSHIFT: u16 = 42;
const KEY_RIGHTSHIFT: u16 = 54;
const KEY_LEFTCTRL: u16 = 29;
const KEY_RIGHTCTRL: u16 = 97;

const EVENTQ_INDEX: u16 = 0;
const EVENTQ_BUFFERS: u16 = 64;

/// Layout mandated by the virtio spec ("Input configuration layout").
/// `select`/`subsel` bank-switch which member of the 128-byte union `u` is
/// currently readable -- see device_name().
#[repr(C, packed)]
struct Config {
    select: u8,
    subsel: u8,
    size: u8,
    reserved: [u8; 5],
    u: [u8; 128],
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct Event {
    kind: u16,
    code: u16,
    value: u32,
}

struct Keyboard {
    eventq: Virtqueue,
    /// HHDM-mapped, one per posted buffer.
    events: *mut Event,
    shift_held: bool,
    ctrl_held: bool,
    /// A key's bytes come from keymap::bytes(); this driver hands out one
    /// byte per poll, so a multi-byte escape sequence returns its first
    /// byte and parks the rest here for the next polls.
    pending: &'static [u8],
}

static KEYBOARD: Global<Option<Keyboard>> = Global::new(None);

/// Reads the device's ID_NAME string via the select/subsel bank-switch --
/// write which field is wanted, then read it out of the union.
fn device_name(cfg: *mut Config, out: &mut [u8; 64]) -> &[u8] {
    vwrite!(cfg, select, CFG_ID_NAME);
    vwrite!(cfg, subsel, 0);
    let size = (vread!(cfg, size) as usize).min(out.len());
    for (i, b) in out.iter_mut().take(size).enumerate() {
        *b = vread!(cfg, u[i]);
    }
    // QEMU counts the string's terminating NUL in `size`.
    let len = out[..size].iter().position(|&b| b == 0).unwrap_or(size);
    &out[..len]
}

impl Keyboard {
    fn post(&mut self, index: u16) {
        let ev = unsafe { self.events.add(index as usize) };
        self.eventq.submit_chain(&[Buffer::from_device(ev, size_of::<Event>())]);
    }
}

/// Finds and starts the keyboard. pci_enumerate() must have run. False if
/// there's no virtio-input keyboard.
pub fn init() -> bool {
    let mut found = None;
    for i in 0.. {
        let Some(pci) = pci::find_device_nth(PCI_VENDOR_ID, PCI_DEVICE_ID, i) else { break };
        let Some(dev) = VirtioDevice::new(pci).filter(|d| !d.device_cfg.is_null()) else { continue };
        let mut buf = [0u8; 64];
        let name = device_name(dev.device_cfg.cast(), &mut buf);
        let name = core::str::from_utf8(name).unwrap_or("?");
        kprintln!("virtio-input: found \"{name}\" at {}:{}.{}", pci.bus, pci.slot, pci.func);
        if name.to_ascii_lowercase().contains("keyboard") {
            found = Some(dev);
            break;
        }
    }
    let Some(dev) = found else {
        kprintln!("virtio-input: no keyboard device found -- boot QEMU with -device virtio-keyboard-pci");
        return false;
    };

    if !dev.negotiate_features(0) {
        return false;
    }
    let Some(eventq) = dev.queue(EVENTQ_INDEX) else { return false };
    dev.driver_ok();

    let page = pmm::alloc_page();
    if page == 0 {
        kprintln!("virtio-input: out of memory");
        return false;
    }
    let events: *mut Event = phys_to_virt(page);
    unsafe { core::ptr::write_bytes(events.cast::<u8>(), 0, PAGE_SIZE as usize) };

    let count = EVENTQ_BUFFERS.min(eventq.size);
    let mut kb = Keyboard { eventq, events, shift_held: false, ctrl_held: false, pending: &[] };
    (0..count).for_each(|i| kb.post(i));
    unsafe { *KEYBOARD.get() = Some(kb) };
    kprintln!("virtio-input: keyboard ready ({count} event buffers posted)");
    true
}

/// Non-blocking: the next byte typed, or None.
pub fn poll_char() -> Option<u8> {
    let kb = unsafe { KEYBOARD.get() }.as_mut()?;
    if let Some((&first, rest)) = kb.pending.split_first() {
        kb.pending = rest;
        return Some(first);
    }
    loop {
        let (id, _) = kb.eventq.try_wait()?;
        let ev = unsafe { kb.events.add(id as usize).read_volatile() };
        kb.post(id); // The same slot the device just filled -- repost immediately.

        let (kind, code, value) = (ev.kind, ev.code, ev.value);
        if kind != EV_KEY {
            continue; // EV_SYN (frame separators) etc.
        }
        match code {
            KEY_LEFTSHIFT | KEY_RIGHTSHIFT => kb.shift_held = value != 0,
            // Tracked like shift. Needed for scarf's vim window commands,
            // which are all Ctrl-w prefixed -- over serial the terminal
            // produces the 0x17 control byte itself, so this only ever
            // mattered in a graphical window.
            KEY_LEFTCTRL | KEY_RIGHTCTRL => kb.ctrl_held = value != 0,
            // Press (1) and repeat (2); not release (0).
            _ if value == 1 || value == 2 => {
                let mods =
                    if kb.shift_held { keymap::MOD_SHIFT } else { 0 } | if kb.ctrl_held { keymap::MOD_CTRL } else { 0 };
                let Some(bytes) = keymap::bytes(code, mods) else { continue };
                kb.pending = &bytes[1..];
                return Some(bytes[0]);
            }
            _ => {}
        }
    }
}
