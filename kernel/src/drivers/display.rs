//! Whatever the screen is, as something to draw into: virtio-gpu when PCI
//! enumeration found one (QEMU), otherwise the firmware's GOP framebuffer
//! that Limine hands over (a real PC, where the graphics card's UEFI
//! driver set it up before boot). The GOP path draws into a RAM copy and
//! copies the changed part to the screen on flush, since reading video
//! memory back over PCIe -- which scrolling does -- is very slow. Nothing
//! drawn is visible until a flush.

use super::virtio::gpu;
use crate::boot::requests::{self, phys_to_virt};
use crate::mm::pmm::{self, PAGE_SIZE};
use crate::sync::Global;
use core::arch::asm;

/// BGRX8888, one u32 per pixel, `pitch` pixels from one row to the next
/// (wider than `width` on a firmware framebuffer, whose rows are often
/// padded).
#[derive(Clone, Copy)]
pub struct Framebuffer {
    pub pixels: *mut u32,
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
}

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    None,
    Virtio,
    Gop,
}

struct Display {
    kind: Kind,
    /// What callers draw into.
    screen: Framebuffer,
    /// GOP only: the real framebuffer, which `screen` is a RAM copy of.
    /// Limine maps it into the HHDM as write-combining, so plain stores to
    /// it are fast; reads are not, and nothing here does any.
    gop_base: *mut u8,
    gop_pitch_bytes: u64,
}

static DISPLAY: Global<Display> = Global::new(Display {
    kind: Kind::None,
    screen: Framebuffer { pixels: core::ptr::null_mut(), width: 0, height: 0, pitch: 0 },
    gop_base: core::ptr::null_mut(),
    gop_pitch_bytes: 0,
});

fn display() -> &'static mut Display {
    unsafe { DISPLAY.get() }
}

const LIMINE_FRAMEBUFFER_RGB: u8 = 1;

fn gop_init() -> bool {
    let Some(fb) = requests::framebuffer().filter(|r| r.framebuffer_count > 0).map(|r| unsafe { &**r.framebuffers })
    else {
        kprintln!("display: no firmware framebuffer from the bootloader");
        return false;
    };

    // BGRX8888 is what everything above this draws, and what UEFI GOP
    // framebuffers are in practice; anything else would need converting on
    // every flush, so it's refused instead.
    if fb.bpp != 32
        || fb.memory_model != LIMINE_FRAMEBUFFER_RGB
        || fb.red_mask_shift != 16
        || fb.green_mask_shift != 8
        || fb.blue_mask_shift != 0
    {
        kprintln!(
            "display: unsupported framebuffer format ({} bpp, R@{} G@{} B@{})",
            fb.bpp,
            fb.red_mask_shift,
            fb.green_mask_shift,
            fb.blue_mask_shift
        );
        return false;
    }

    let bytes = fb.width * fb.height * 4;
    let shadow = pmm::alloc_pages(bytes.div_ceil(PAGE_SIZE));
    if shadow == 0 {
        kprintln!("display: no memory for a {} KiB framebuffer copy", bytes / 1024);
        return false;
    }
    let d = display();
    d.screen = Framebuffer {
        pixels: phys_to_virt(shadow),
        width: fb.width as u32,
        height: fb.height as u32,
        pitch: fb.width as u32,
    };
    unsafe { core::ptr::write_bytes(d.screen.pixels.cast::<u8>(), 0, bytes as usize) };
    d.gop_base = fb.address.cast();
    d.gop_pitch_bytes = fb.pitch;
    kprintln!("display: GOP framebuffer {}x{}, pitch {} bytes", d.screen.width, d.screen.height, d.gop_pitch_bytes);
    true
}

/// Picks the display. pci_enumerate() and the PMM must be up. The
/// framebuffer to draw into, or None if there's no screen at all.
pub fn init() -> Option<Framebuffer> {
    let d = display();
    if crate::drivers::pci::find_device(gpu::PCI_VENDOR_ID, gpu::PCI_DEVICE_ID).is_some()
        && let Some(fb) = gpu::init()
    {
        d.screen = fb;
        d.kind = Kind::Virtio;
    } else if gop_init() {
        d.kind = Kind::Gop;
    } else {
        return None;
    }
    Some(d.screen)
}

/// "virtio-gpu" or "GOP framebuffer"; None before a successful init.
pub fn name() -> Option<&'static str> {
    match display().kind {
        Kind::Virtio => Some("virtio-gpu"),
        Kind::Gop => Some("GOP framebuffer"),
        Kind::None => None,
    }
}

pub fn flush() {
    let s = display().screen;
    flush_rect(0, 0, s.width, s.height);
}

/// Makes just this rectangle visible -- what a console that only changed
/// a few characters calls. Clipped to the screen.
pub fn flush_rect(x: u32, y: u32, w: u32, h: u32) {
    let d = display();
    if d.kind == Kind::Virtio {
        gpu::flush(); // Always the whole screen; cheap for QEMU.
        return;
    }
    let s = d.screen;
    if d.kind != Kind::Gop || x >= s.width || y >= s.height {
        return;
    }
    let w = w.min(s.width - x);
    let h = h.min(s.height - y);

    // One `rep movsb` per row: fast-string moves are the quickest way to
    // stream into write-combining memory without SSE, which the kernel
    // isn't allowed to touch.
    for row in y..y + h {
        let src = unsafe { s.pixels.add(row as usize * s.pitch as usize + x as usize) };
        let dst = unsafe { d.gop_base.add(row as usize * d.gop_pitch_bytes as usize + x as usize * 4) };
        unsafe {
            asm!("rep movsb", inout("rdi") dst => _, inout("rsi") src => _, inout("rcx") w as usize * 4 => _, options(nostack, preserves_flags))
        };
    }
}
