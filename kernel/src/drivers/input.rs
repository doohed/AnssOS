//! Every keyboard the kernel can read, behind one poll: the virtio-input
//! keyboard (QEMU), USB keyboards (drivers/usb/kbd.rs) and COM1.
//! Sources that aren't present just report nothing, so this is safe to
//! call whatever got initialized.

use super::{serial, usb, virtio};

/// Non-blocking: the next byte, or None. Keys with no single byte come
/// out as the escape sequences a VT100-style terminal sends (ESC [ A for
/// Up, ...), the same from every source.
pub fn poll_char() -> Option<u8> {
    virtio::input::poll_char().or_else(usb::kbd::poll_char).or_else(serial::poll_char)
}
