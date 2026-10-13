//! The kernel's tick: TIMER_HZ interrupts a second on vector PIC_IRQ_BASE
//! (dispatched as "IRQ0" by arch/x86_64/idt.rs, which is also what drives
//! preemption). Sources, best first:
//!
//!   1. The Local APIC timer, calibrated against the ACPI PM timer (or PIT
//!      channel 2, polled, if there's no PM timer). It's inside the CPU,
//!      so no interrupt routing is involved -- what makes it work on a
//!      real PC whose board doesn't wire the old 8259 to the CPU.
//!   2. PIT channel 0 through the 8259, as on QEMU.
//!   3. None: check() found no ticks arriving at all. Sleeps become
//!      busy-waits and there's no preemption, but boot carries on.
//!
//! init() picks 1 or 2 (after pic::remap(), before `sti`); check() (right
//! after `sti`) confirms ticks really arrive and steps down the list if
//! not.

use core::arch::asm;
use core::sync::atomic::{AtomicU64, Ordering};

use super::{acpi, pit};
use crate::arch::x86_64::idt;
use crate::arch::x86_64::pic::{self, PIC_IRQ_BASE};
use crate::arch::x86_64::{cpuid, lapic, rdtsc};
use crate::sync::Global;

/// 100 ticks a second, 10 ms each.
pub const TIMER_HZ: u64 = 100;

const CALIBRATE_MS: u32 = 50;
const LAPIC_TIMER_PERIODIC: u32 = 1 << 17;
const LAPIC_LVT_MASKED: u32 = 1 << 16;
const LAPIC_DIVIDE_BY_16: u32 = 0x3;

#[derive(Clone, Copy, PartialEq)]
enum Source {
    None,
    Lapic,
    Pit,
}

struct Timer {
    source: Source,
    lapic_counts_per_tick: u32,
    calibrated_against: &'static str,
    /// Whether the LAPIC timer keeps running while the CPU sleeps in `hlt`
    /// (CPUID "ARAT"). Without it, `hlt` could wait forever for a tick
    /// that the sleeping CPU stopped counting towards.
    lapic_always_running: bool,
}

static TICKS: AtomicU64 = AtomicU64::new(0);
static TIMER: Global<Timer> = Global::new(Timer {
    source: Source::None,
    lapic_counts_per_tick: 0,
    calibrated_against: "",
    lapic_always_running: false,
});

fn timer() -> &'static mut Timer {
    unsafe { TIMER.get() }
}

fn tick() {
    TICKS.fetch_add(1, Ordering::Relaxed);
}

/// Busy-waits `ms` on whatever needs no interrupts: the ACPI PM timer, or
/// PIT channel 2. False if neither works.
fn poll_wait_ms(mut ms: u32) -> bool {
    if acpi::pm_timer_present() {
        let target = (acpi::PM_TIMER_HZ as u64 * ms as u64 / 1000) as u32;
        let start = acpi::pm_timer_read();
        // Bounded like pit::poll_wait_ms(): a stuck counter gives up after
        // a few seconds' worth of port reads.
        return (0..5_000_000 + ms as u64 * 5000).any(|_| acpi::pm_timer_delta(start, acpi::pm_timer_read()) >= target);
    }
    while ms > CALIBRATE_MS {
        if !pit::poll_wait_ms(CALIBRATE_MS) {
            return false;
        }
        ms -= CALIBRATE_MS;
    }
    pit::poll_wait_ms(ms)
}

/// Counts how far the LAPIC timer gets in CALIBRATE_MS, then runs it
/// periodically at TIMER_HZ. False if it can't be measured.
fn start_lapic_timer() -> bool {
    lapic::init();
    lapic::write(lapic::REG_TIMER_DIVIDE, LAPIC_DIVIDE_BY_16);
    lapic::write(lapic::REG_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic::write(lapic::REG_TIMER_INIT, 0xFFFF_FFFF);
    let waited = poll_wait_ms(CALIBRATE_MS);
    let elapsed = 0xFFFF_FFFF - lapic::read(lapic::REG_TIMER_CURRENT);
    lapic::write(lapic::REG_TIMER_INIT, 0);
    if !waited {
        kprintln!("timer: nothing to calibrate the LAPIC timer against");
        return false;
    }

    let per_tick = elapsed as u64 * 1000 / TIMER_HZ / CALIBRATE_MS as u64;
    if !(100..=0xFFFF_FFFF).contains(&per_tick) {
        kprintln!("timer: LAPIC timer counted {elapsed} in {CALIBRATE_MS} ms -- not usable");
        return false;
    }
    let t = timer();
    t.lapic_counts_per_tick = per_tick as u32;
    t.calibrated_against = if acpi::pm_timer_present() { "the ACPI PM timer" } else { "PIT channel 2" };
    t.lapic_always_running = (cpuid(6).0 >> 2) & 1 != 0;

    lapic::write(lapic::REG_LVT_TIMER, PIC_IRQ_BASE as u32 | LAPIC_TIMER_PERIODIC);
    lapic::write(lapic::REG_TIMER_INIT, t.lapic_counts_per_tick);
    true
}

fn start_pit() {
    pit::start_periodic(TIMER_HZ as u32);
    pic::clear_mask(0);
}

pub fn init() {
    idt::irq_register(0, tick);
    acpi::init();
    timer().source = if start_lapic_timer() {
        Source::Lapic // IRQ0 stays masked on the 8259: one tick source only.
    } else {
        start_pit();
        Source::Pit
    };
}

/// Whether a few ticks arrive within about 200 ms. The deadline is
/// measured without interrupts -- by the PM timer if there is one,
/// otherwise by a TSC count that's 200 ms or more at any clock speed a PC
/// running this has (it only has to bound the wait).
fn ticks_arriving() -> bool {
    let start = ticks();
    let have_pm = acpi::pm_timer_present();
    let pm_start = if have_pm { acpi::pm_timer_read() } else { 0 };
    let tsc_start = rdtsc();
    loop {
        if ticks() - start >= 3 {
            return true;
        }
        let expired = if have_pm {
            acpi::pm_timer_delta(pm_start, acpi::pm_timer_read()) > acpi::PM_TIMER_HZ / 5
        } else {
            rdtsc() - tsc_start > 1_000_000_000
        };
        if expired {
            return false;
        }
        core::hint::spin_loop();
    }
}

pub fn check() {
    if ticks_arriving() {
        return;
    }
    let t = timer();
    if t.source == Source::Lapic {
        kprintln!("timer: LAPIC timer ticks aren't arriving -- trying the PIT");
        lapic::write(lapic::REG_LVT_TIMER, LAPIC_LVT_MASKED);
        start_pit();
        t.source = Source::Pit;
        if ticks_arriving() {
            return;
        }
    }
    kprintln!("timer: PIT ticks aren't arriving -- no timer interrupts at all");
    pic::set_mask(0);
    t.source = Source::None;
}

/// For idt.rs: whether the tick needs a Local APIC EOI instead of an 8259
/// one.
pub fn uses_lapic() -> bool {
    timer().source == Source::Lapic
}

pub fn ticks() -> u64 {
    TICKS.load(Ordering::Relaxed)
}

pub fn uptime_ms() -> u64 {
    ticks() * (1000 / TIMER_HZ)
}

/// Waits at least `ms` milliseconds (sti; hlt between ticks, so it can't
/// yield to anything else).
pub fn sleep_ms(ms: u32) {
    if ms == 0 {
        return;
    }
    let t = timer();
    if t.source == Source::None {
        poll_wait_ms(ms);
        return;
    }
    let ms_per_tick = (1000 / TIMER_HZ) as u32;
    let target = ticks() + ms.div_ceil(ms_per_tick) as u64;
    while ticks() < target {
        if t.source == Source::Lapic && !t.lapic_always_running {
            unsafe { asm!("sti; pause", options(nomem, nostack)) };
        } else {
            unsafe { asm!("sti; hlt", options(nomem, nostack)) };
        }
    }
}

/// One moment of a busy-wait: lets a pending tick in, then puts the
/// interrupt flag back the way it was. Inside a syscall (an interrupt
/// gate: interrupts off) a long wait would otherwise stop the clock --
/// uptime freezes and timeouts measured with uptime_ms() never expire.
pub fn idle() {
    let was_enabled = crate::arch::x86_64::interrupts_enabled();
    unsafe { asm!("sti; pause", options(nomem, nostack)) };
    if !was_enabled {
        unsafe { asm!("cli", options(nomem, nostack)) };
    }
}

/// One line describing the source in use -- for the boot log, where it's
/// the first thing to check if a real PC hangs at boot.
pub fn log_status() {
    let t = timer();
    match t.source {
        Source::Lapic => kprintln!(
            "Timer: LAPIC timer ({}), {} counts/tick vs {}, {}",
            if lapic::is_x2apic() { "x2APIC" } else { "xAPIC" },
            t.lapic_counts_per_tick,
            t.calibrated_against,
            if t.lapic_always_running { "ARAT" } else { "no ARAT" }
        ),
        Source::Pit => kprintln!("Timer: PIT through the 8259"),
        Source::None => kprintln!("Timer: NONE -- no timer interrupts; sleeps busy-wait, no preemption"),
    }
}
