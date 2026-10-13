//! Memory a device reads and writes by physical address: whole, zeroed,
//! physically contiguous pages from the PMM, reached through the HHDM.

use super::pmm::{self, PAGE_SIZE};
use crate::boot::requests::phys_to_virt;

#[derive(Clone, Copy)]
pub struct Dma {
    pub virt: *mut u8,
    pub phys: u64,
}

impl Dma {
    pub fn as_ptr<T>(&self) -> *mut T {
        self.virt.cast()
    }
}

/// `pages` zeroed pages, or None when out of memory.
pub fn alloc(pages: u64) -> Option<Dma> {
    let phys = if pages == 1 { pmm::alloc_page() } else { pmm::alloc_pages(pages) };
    if phys == 0 {
        return None;
    }
    let virt: *mut u8 = phys_to_virt(phys);
    unsafe { core::ptr::write_bytes(virt, 0, (pages * PAGE_SIZE) as usize) };
    Some(Dma { virt, phys })
}
