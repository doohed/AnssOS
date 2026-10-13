//! COM1, used both as a debug console and as an input source, plus the
//! kernel log itself: kprint!/kprintln!, which reach serial and, once
//! it's up, the framebuffer console (console/fbconsole.rs).

use core::fmt;

use crate::arch::x86_64::io::{inb, outb};
use crate::sync::Global;

const COM1: u16 = 0x3F8;

/// Whether a UART actually answers at COM1. Many real PCs have none (or
/// only an unpopulated header), and a missing one reads back 0xFF from
/// every register -- whose Line Status bit 0 says "data ready" forever,
/// which would feed the shell an endless stream of phantom 0xFF bytes.
static PRESENT: Global<bool> = Global::new(false);

pub fn init() {
    outb(COM1 + 1, 0x00); // Disable interrupts; we poll.
    outb(COM1 + 3, 0x80); // Enable DLAB to set the baud rate divisor.
    outb(COM1, 0x03); // Divisor low byte: 38400 baud.
    outb(COM1 + 1, 0x00); // Divisor high byte.
    outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit; clear DLAB.
    outb(COM1 + 2, 0xC7); // Enable FIFO, clear it, 14-byte threshold.
    outb(COM1 + 4, 0x0B); // IRQs off, RTS/DSR set.

    // Loopback self-test: in loopback mode a real 16550 hands a sent byte
    // straight back to its own receiver (and not out of the port).
    outb(COM1 + 4, 0x1E);
    outb(COM1, 0xAE);
    unsafe { *PRESENT.get() = inb(COM1) == 0xAE };
    outb(COM1 + 4, 0x0F); // Back to normal operation, OUT1/OUT2 set.
}

fn present() -> bool {
    unsafe { *PRESENT.get() }
}

/// Non-blocking: the next byte received on COM1, or None -- always None
/// when serial_init() found no UART there. A second input source
/// alongside the keyboards: QEMU's `-serial stdio` wires it straight to
/// the host terminal, so typing there reaches the shell like a normal
/// serial console, even headless.
pub fn poll_char() -> Option<u8> {
    // Line Status Register bit 0: Data Ready.
    if !present() || inb(COM1 + 5) & 0x01 == 0 {
        return None;
    }
    Some(inb(COM1))
}

pub fn putc(c: u8) {
    if !present() {
        return;
    }
    if c == b'\n' {
        putc(b'\r');
    }
    while inb(COM1 + 5) & 0x20 == 0 {} // Wait for the transmit buffer to empty.
    outb(COM1, c);
}

/// `core::fmt` sink for serial alone -- for output that mustn't reach the
/// screen.
pub struct Serial;

impl fmt::Write for Serial {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        s.bytes().for_each(putc);
        Ok(())
    }
}

/// A second output for everything the kernel logs -- the framebuffer
/// console, once it's up.
static CONSOLE_SINK: Global<Option<fn(u8)>> = Global::new(None);
static CONSOLE_MUTED: Global<i32> = Global::new(0);

/// Registers a second output for everything the kernel logs -- the
/// framebuffer console, once it's up.
pub fn set_console_sink(sink: fn(u8)) {
    unsafe { *CONSOLE_SINK.get() = Some(sink) };
}

/// While muted (calls nest), the log still reaches serial but not the
/// console. For kernel code that reports its own errors to the log -- the
/// vfs's -- when it runs on a userland program's behalf (a syscall): the
/// program reports errors itself, and the screen is the program's,
/// possibly a tile pane drawn by someone else.
pub fn mute_console(mute: bool) {
    unsafe { *CONSOLE_MUTED.get() += if mute { 1 } else { -1 } };
}

/// Every log byte goes through here, so it reaches both the serial port
/// and, once registered, the framebuffer console.
pub fn kout(c: u8) {
    putc(c);
    let (sink, muted) = unsafe { (*CONSOLE_SINK.get(), *CONSOLE_MUTED.get()) };
    if let Some(sink) = sink
        && muted == 0
    {
        sink(c);
    }
}

/// `core::fmt` sink for the kernel log.
pub struct Kout;

impl fmt::Write for Kout {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        s.bytes().for_each(kout);
        Ok(())
    }
}

/// Prints to the kernel log: serial, and the screen once it's up.
macro_rules! kprint {
    ($($arg:tt)*) => {{
        let _ = core::fmt::Write::write_fmt(&mut $crate::drivers::serial::Kout, format_args!($($arg)*));
    }};
}

macro_rules! kprintln {
    () => { kprint!("\n") };
    ($($arg:tt)*) => { kprint!("{}\n", format_args!($($arg)*)) };
}
