//! The kernel heap: a simple first-fit allocator on top of the PMM, as
//! Rust's global allocator -- what Box, Vec and String allocate from, for
//! everything the PMM alone can't provide (page granularity only). Grows
//! on demand by pulling whole pages from the PMM. No backward coalescing
//! on free -- a documented fragmentation trade-off, not a correctness
//! issue.
//!
//! Every payload is 16-byte aligned; Rust types that need more are
//! over-allocated, with the block's real start stashed in the 8 bytes
//! right below the aligned pointer.

use core::alloc::{GlobalAlloc, Layout};
use core::ptr::null_mut;

use super::pmm::{self, PAGE_SIZE};
use crate::boot::requests::phys_to_virt;
use crate::sync::Global;

const HEAP_ALIGN: usize = 16;

/// 32 bytes, so the payload right after it stays 16-byte aligned.
#[repr(C, align(16))]
struct Block {
    /// Next block in address order (both free and used).
    next: *mut Block,
    /// Usable size, not counting this header.
    size: usize,
    free: bool,
}

const HEADER: usize = size_of::<Block>();

struct Heap {
    /// First block ever, address order.
    head: *mut Block,
    /// Last block -- O(1) append when growing.
    tail: *mut Block,
}

static HEAP: Global<Heap> = Global::new(Heap { head: null_mut(), tail: null_mut() });

fn align_up(n: usize, align: usize) -> usize {
    (n + align - 1) & !(align - 1)
}

impl Heap {
    /// Pulls enough whole pages from the PMM for `min_size` usable bytes
    /// plus a header, as one new free block appended to the list.
    fn grow(&mut self, min_size: usize) -> *mut Block {
        let pages = (HEADER + min_size).div_ceil(PAGE_SIZE as usize);
        let phys = pmm::alloc_pages(pages as u64);
        if phys == 0 {
            return null_mut();
        }
        let block: *mut Block = phys_to_virt(phys);
        unsafe { block.write(Block { next: null_mut(), size: pages * PAGE_SIZE as usize - HEADER, free: true }) };
        if self.tail.is_null() {
            self.head = block;
        } else {
            unsafe { (*self.tail).next = block };
        }
        self.tail = block;
        block
    }

    /// Carves `size` bytes off the front of `block`, the remainder becoming
    /// a new free block right after it -- unless the remainder is too small
    /// to be worth tracking, in which case the slack just stays attached to
    /// this allocation.
    fn split(&mut self, block: *mut Block, size: usize) {
        let b = unsafe { &mut *block };
        if b.size < size + HEADER + HEAP_ALIGN {
            return;
        }
        let rest = (block as usize + HEADER + size) as *mut Block;
        unsafe { rest.write(Block { next: b.next, size: b.size - size - HEADER, free: true }) };
        b.size = size;
        b.next = rest;
        if self.tail == block {
            self.tail = rest;
        }
    }

    fn malloc(&mut self, size: usize) -> *mut u8 {
        let size = align_up(size.max(1), HEAP_ALIGN);
        let mut b = self.head;
        while !b.is_null() && !unsafe { (*b).free && (*b).size >= size } {
            b = unsafe { (*b).next };
        }
        if b.is_null() {
            b = self.grow(size);
            if b.is_null() {
                return null_mut();
            }
        }
        self.split(b, size);
        unsafe { (*b).free = false };
        (b as usize + HEADER) as *mut u8
    }

    fn free(&mut self, ptr: *mut u8) {
        let block = (ptr as usize - HEADER) as *mut Block;
        let b = unsafe { &mut *block };
        b.free = true;

        // Forward coalescing only, and only when the next block in the list
        // is truly adjacent in memory -- blocks from different grow() calls
        // aren't necessarily contiguous, since pmm::alloc_pages() just
        // returns *a* free run, not one adjacent to the last allocation.
        let next = b.next;
        if !next.is_null() && unsafe { (*next).free } && next as usize == block as usize + HEADER + b.size {
            let n = unsafe { &*next };
            b.size += HEADER + n.size;
            b.next = n.next;
            if self.tail == next {
                self.tail = block;
            }
        }
    }
}

fn heap() -> &'static mut Heap {
    unsafe { HEAP.get() }
}

struct KernelAlloc;

unsafe impl GlobalAlloc for KernelAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let align = layout.align();
        if align <= HEAP_ALIGN {
            return heap().malloc(layout.size());
        }
        // The payload is 16-aligned and `align` a larger power of two, so
        // the first aligned address at least 8 bytes in is at most
        // `align - 8` bytes in: `align` extra bytes always cover it.
        let raw = heap().malloc(layout.size() + align);
        if raw.is_null() {
            return raw;
        }
        let aligned = align_up(raw as usize + 8, align);
        unsafe { ((aligned - 8) as *mut usize).write(raw as usize) };
        aligned as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        let raw = if layout.align() <= HEAP_ALIGN {
            ptr
        } else {
            unsafe { *((ptr as usize - 8) as *const usize) as *mut u8 }
        };
        heap().free(raw);
    }
}

#[global_allocator]
static ALLOC: KernelAlloc = KernelAlloc;
