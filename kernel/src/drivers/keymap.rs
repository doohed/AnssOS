//! US QWERTY. Turns a key press, as a Linux key code (the kernel's
//! input-event-codes.h -- what virtio-input reports, and what the USB
//! keyboard driver converts HID usages to), into the bytes a VT100-style
//! terminal would send for it: one byte for most keys, ESC [ ... for
//! arrows, Home/End, Delete and Page Up/Down, Ctrl-A..Ctrl-Z as
//! 0x01-0x1a. Every keyboard driver goes through this, so a program sees
//! identical bytes whichever keyboard -- or the serial console -- it's
//! reading.

use crate::sync::Global;

pub const MOD_SHIFT: i32 = 1;
pub const MOD_CTRL: i32 = 2;
/// Caps Lock on: letters flip case.
pub const MOD_CAPS: i32 = 4;

/// Keys with no single byte come out as the escape sequences a
/// VT100-style terminal sends for them, exactly what a program reading a
/// serial terminal already sees.
const SPECIAL_KEYS: [(u16, &[u8]); 9] = [
    (102, b"\x1b[H"),  // Home
    (103, b"\x1b[A"),  // Up
    (104, b"\x1b[5~"), // Page Up
    (105, b"\x1b[D"),  // Left
    (106, b"\x1b[C"),  // Right
    (107, b"\x1b[F"),  // End
    (108, b"\x1b[B"),  // Down
    (109, b"\x1b[6~"), // Page Down
    (111, b"\x1b[3~"), // Delete
];

/// Linux key codes -> ASCII, 0 meaning "no mapping, drop the key".
///
/// Escape (code 1) matters more than it looks: it's the only way out of
/// insert mode in scarf, and without it modal editing is impossible on a
/// keyboard. The keypad (55, 71-83, 96, 98) types its digits and symbols,
/// as if Num Lock were on.
const LOWER: [u8; 99] = {
    let mut m = [0u8; 99];
    row(&mut m, 1, b"\x1b1234567890-=\x08\tqwertyuiop[]\n");
    row(&mut m, 30, b"asdfghjkl;'`");
    row(&mut m, 43, b"\\zxcvbnm,./");
    m[55] = b'*';
    m[57] = b' ';
    row(&mut m, 71, b"789-456+1230.");
    m[96] = b'\n';
    m[98] = b'/';
    m
};

/// The shifted half of the same layout. Codes absent here fall back to
/// LOWER (with a-z uppercased), so an unshifted key never stops working
/// just because its shifted form is unlisted.
const UPPER: [u8; 54] = {
    let mut m = [0u8; 54];
    row(&mut m, 2, b"!@#$%^&*()_+");
    m[26] = b'{';
    m[27] = b'}';
    m[39] = b':';
    m[40] = b'"';
    m[41] = b'~';
    m[43] = b'|';
    m[51] = b'<';
    m[52] = b'>';
    m[53] = b'?';
    m
};

/// Copies `keys` into consecutive codes from `start` -- a row of keys.
const fn row(m: &mut [u8], start: usize, keys: &[u8]) {
    let mut i = 0;
    while i < keys.len() {
        m[start + i] = keys[i];
        i += 1;
    }
}

/// The one-byte answer -- valid until the next call.
static ONE: Global<[u8; 1]> = Global::new([0; 1]);

/// The bytes for key `code` with modifiers `mods` (MOD_*), or None for
/// keys that produce nothing (modifiers, function keys, ...).
pub fn bytes(code: u16, mods: i32) -> Option<&'static [u8]> {
    if let Some((_, seq)) = SPECIAL_KEYS.iter().find(|(c, _)| *c == code) {
        return Some(seq);
    }
    let mut c = *LOWER.get(code as usize)?;
    let letter = c.is_ascii_lowercase();
    let shift = mods & MOD_SHIFT != 0;
    if mods & MOD_CTRL != 0 && letter {
        c = c - b'a' + 1; // Ctrl-A..Ctrl-Z -> 0x01..0x1a, as a terminal sends.
    } else if letter {
        if shift != (mods & MOD_CAPS != 0) {
            c = c.to_ascii_uppercase();
        }
    } else if shift && let Some(&upper) = UPPER.get(code as usize).filter(|&&u| u != 0) {
        c = upper;
    }
    if c == 0 {
        return None;
    }
    let one = unsafe { ONE.get() };
    *one = [c];
    Some(one)
}
