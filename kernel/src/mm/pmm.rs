//! Bitmap physical page allocator, built from Limine's memory map. Must
//! be initialized after the HHDM and memmap Limine requests have valid
//! responses (see boot/requests.rs), since it needs both to place and
//! address the bitmap itself.

use crate::arch::x86_64::halt_forever;
use crate::boot::requests::{self, MEMMAP_BOOTLOADER_RECLAIMABLE, MEMMAP_USABLE};
use crate::sync::Global;

pub const PAGE_SIZE: u64 = 4096;

struct Pmm {
    bitmap: *mut u8,
    total_pages: u64,
    free_pages: u64,
    /// Where the next single-page search starts: just past the last page
    /// handed out.
    cursor: u64,
}

static PMM: Global<Pmm> = Global::new(Pmm { bitmap: core::ptr::null_mut(), total_pages: 0, free_pages: 0, cursor: 0 });

impl Pmm {
    fn test(&self, page: u64) -> bool {
        unsafe { *self.bitmap.add((page / 8) as usize) & (1 << (page % 8)) != 0 }
    }

    fn set(&mut self, page: u64) {
        unsafe { *self.bitmap.add((page / 8) as usize) |= 1 << (page % 8) };
    }

    fn clear(&mut self, page: u64) {
        unsafe { *self.bitmap.add((page / 8) as usize) &= !(1 << (page % 8)) };
    }

    fn try_alloc_from(&mut self, start: u64, end: u64) -> u64 {
        for page in start..end {
            if !self.test(page) {
                self.set(page);
                self.free_pages -= 1;
                self.cursor = page + 1;
                return page * PAGE_SIZE;
            }
        }
        0
    }
}

fn pmm() -> &'static mut Pmm {
    unsafe { PMM.get() }
}

pub fn init() {
    let (Some(mm), Some(_)) = (requests::memmap(), requests::hhdm()) else {
        kprintln!("PMM: PANIC: missing memmap or HHDM response from bootloader");
        halt_forever();
    };
    let p = pmm();

    let highest = mm
        .entries()
        .filter(|e| e.kind == MEMMAP_USABLE || e.kind == MEMMAP_BOOTLOADER_RECLAIMABLE)
        .map(|e| e.base + e.length)
        .max()
        .unwrap_or(0);
    p.total_pages = highest / PAGE_SIZE;
    let bitmap_bytes = p.total_pages.div_ceil(8);

    // Park the bitmap itself in the first usable region big enough to hold
    // it, and reach it through the HHDM. (Address 0 is a legitimate region
    // base -- the first usable entry commonly starts there -- so this is
    // an Option, not a "0 means not found" address.)
    let Some(bitmap_phys) = mm.entries().find(|e| e.kind == MEMMAP_USABLE && e.length >= bitmap_bytes).map(|e| e.base)
    else {
        kprintln!("PMM: PANIC: no usable region large enough for the page bitmap");
        halt_forever();
    };
    p.bitmap = requests::phys_to_virt(bitmap_phys);
    unsafe { core::ptr::write_bytes(p.bitmap, 0xFF, bitmap_bytes as usize) }; // Start with everything used.

    p.free_pages = 0;
    for e in mm.entries().filter(|e| e.kind == MEMMAP_USABLE) {
        let start = e.base / PAGE_SIZE;
        for page in start..start + e.length / PAGE_SIZE {
            p.clear(page);
            p.free_pages += 1;
        }
    }

    // The region just marked free includes the pages the bitmap lives in
    // -- claw those back.
    let start = bitmap_phys / PAGE_SIZE;
    for page in start..start + bitmap_bytes.div_ceil(PAGE_SIZE) {
        if !p.test(page) {
            p.set(page);
            p.free_pages -= 1;
        }
    }

    // Page 0 is never handed out: 0 is what every allocator here returns
    // for "out of memory". QEMU's firmware never reports it as usable,
    // but a real PC's commonly does (the first 640 KiB), and handing it
    // out made the heap's very first allocation look like a failure.
    if !p.test(0) {
        p.set(0);
        p.free_pages -= 1;
    }

    p.cursor = 0;
    kprintln!("PMM: {} pages tracked, {} MiB free", p.total_pages, p.free_pages * PAGE_SIZE / (1024 * 1024));
}

/// A physical page's address, or 0 when out of memory -- 0 is never a
/// valid allocation since init() always reserves physical page 0.
pub fn alloc_page() -> u64 {
    let p = pmm();
    let addr = p.try_alloc_from(p.cursor, p.total_pages);
    if addr != 0 || p.cursor == 0 {
        return addr;
    }
    // Wrap around: pages before the cursor may have been freed since.
    let cursor = p.cursor;
    p.try_alloc_from(0, cursor)
}

pub fn free_page(phys: u64) {
    let p = pmm();
    let page = phys / PAGE_SIZE;
    if page >= p.total_pages || !p.test(page) {
        return; // Out of range or double free -- ignore.
    }
    p.clear(page);
    p.free_pages += 1;
}

/// `count` physically contiguous pages (e.g. for a DMA framebuffer that
/// needs to be one linear range), or 0. There's no way to free a run yet
/// -- nothing frees a contiguous range once allocated.
pub fn alloc_pages(count: u64) -> u64 {
    let p = pmm();
    if count == 0 || count > p.total_pages {
        return 0;
    }
    let mut run = 0;
    for page in 0..p.total_pages {
        if p.test(page) {
            run = 0;
            continue;
        }
        run += 1;
        if run == count {
            let start = page + 1 - count;
            (start..=page).for_each(|pg| p.set(pg));
            p.free_pages -= count;
            return start * PAGE_SIZE;
        }
    }
    0
}

pub fn free_page_count() -> u64 {
    pmm().free_pages
}

pub fn total_page_count() -> u64 {
    pmm().total_pages
}
