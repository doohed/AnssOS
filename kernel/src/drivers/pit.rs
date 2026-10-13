//! The legacy 8254 Programmable Interval Timer. drivers/timer.rs is what
//! the rest of the kernel uses; this is just the two things it needs from
//! the chip.

use crate::arch::x86_64::io::{inb, outb};

pub const BASE_FREQ: u32 = 1_193_182;

const CHANNEL0: u16 = 0x40;
const CHANNEL2: u16 = 0x42;
const COMMAND: u16 = 0x43;
/// Bit 0: channel 2 gate, bit 1: speaker, bit 5: channel 2 output.
const GATE_PORT: u16 = 0x61;

/// Channel 0 as a square wave at `hz`: IRQ0 on the 8259, if the board
/// actually routes that to the CPU (QEMU does; real PCs may not).
pub fn start_periodic(hz: u32) {
    let divisor = (BASE_FREQ / hz) as u16;
    outb(COMMAND, 0x36); // Channel 0, lobyte/hibyte access, mode 3 (square wave), binary.
    outb(CHANNEL0, divisor as u8);
    outb(CHANNEL0, (divisor >> 8) as u8);
}

/// Busy-waits `ms` (at most 50) on channel 2, polled through port 0x61 --
/// no interrupt involved. False if the channel never finishes, e.g. a
/// chipset with the 8254 switched off.
pub fn poll_wait_ms(ms: u32) -> bool {
    let count = (BASE_FREQ / 1000 * ms).min(0xFFFF) as u16;

    // Gate off, speaker off, then mode 0 (output goes high when the count
    // reaches zero), then raise the gate to start counting.
    let gate = inb(GATE_PORT) & !0x03;
    outb(GATE_PORT, gate);
    outb(COMMAND, 0xB0); // Channel 2, lobyte/hibyte access, mode 0, binary.
    outb(CHANNEL2, count as u8);
    outb(CHANNEL2, (count >> 8) as u8);
    outb(GATE_PORT, gate | 0x01);

    // Each port read is roughly a microsecond, so this gives up after a
    // few seconds rather than hanging on a dead timer.
    let done = (0..5_000_000).any(|_| inb(GATE_PORT) & 0x20 != 0);
    outb(GATE_PORT, gate);
    done
}
