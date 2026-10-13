//! Virtual memory: a real 4-level page-table walker, used for two things
//! -- mapping arbitrary physical MMIO ranges (PCI BARs) into the kernel's
//! own address space, and (M10) building separate, per-user-task address
//! spaces for ring-3 execution. Everything else still rides on Limine's
//! HHDM. There's no unmapping and no demand paging here.

use super::pmm::{self, PAGE_SIZE};
use crate::boot::requests::phys_to_virt;
use crate::sync::Global;
use core::arch::asm;

pub const PAGE_PRESENT: u64 = 1 << 0;
pub const PAGE_WRITABLE: u64 = 1 << 1;
/// Ring-3-accessible; see map().
pub const PAGE_USER: u64 = 1 << 2;
/// PCD -- required for correct MMIO semantics.
pub const PAGE_NOCACHE: u64 = 1 << 4;

const PAGE_ADDR_MASK: u64 = 0x000f_ffff_ffff_f000;
const ENTRIES: usize = 512;

/// A page-table hierarchy, identified by its top-level (PML4) physical
/// address -- what CR3 holds while that address space is active.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AddrSpace {
    pub pml4_phys: u64,
}

/// Bump allocator for MMIO virtual addresses: a dedicated slice of the
/// higher half, comfortably below the kernel image (0xffffffff80000000)
/// and far above the HHDM (which starts at the HHDM offset, covering only
/// actual RAM). Mappings are never reused or freed.
static NEXT_MMIO_VIRT: Global<u64> = Global::new(0xffff_ff80_0000_0000);

fn table(phys: u64) -> &'static mut [u64; ENTRIES] {
    unsafe { &mut *phys_to_virt(phys) }
}

fn current_cr3() -> u64 {
    let cr3: u64;
    unsafe { asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack, preserves_flags)) };
    cr3 & PAGE_ADDR_MASK
}

/// Intermediate (non-leaf) table entries are always present+writable+user,
/// regardless of what the eventual leaf mapping needs: the CPU ANDs the
/// U/S bit across every level of the walk, so an intermediate entry
/// without it would make a user-accessible leaf unreachable from ring 3.
/// This doesn't weaken protection -- what's actually reachable is still
/// gated by the leaf PTE's own flags, which map()'s caller controls; a
/// purely-kernel subtree simply never gets a leaf with PAGE_USER set.
fn get_or_create(parent: &mut [u64; ENTRIES], index: usize) -> &'static mut [u64; ENTRIES] {
    if parent[index] & PAGE_PRESENT == 0 {
        let phys = pmm::alloc_page();
        table(phys).fill(0);
        parent[index] = phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }
    table(parent[index] & PAGE_ADDR_MASK)
}

fn walk_create(pml4_phys: u64, virt: u64) -> &'static mut u64 {
    let index = |shift: u32| ((virt >> shift) & 0x1FF) as usize;
    let pdpt = get_or_create(table(pml4_phys), index(39));
    let pd = get_or_create(pdpt, index(30));
    let pt = get_or_create(pd, index(21));
    &mut pt[index(12)]
}

/// The address space active at boot and used by the kernel itself (read
/// from CR3) -- every fresh one from new_address_space() shares its
/// higher half (kernel code, HHDM, MMIO) automatically.
pub fn current_address_space() -> AddrSpace {
    AddrSpace { pml4_phys: current_cr3() }
}

/// Allocates a fresh top-level page table, zeroes it, then copies PML4
/// entries 256-511 (the whole canonical higher half) from the currently
/// active address space -- kernel code/data, the HHDM, and any existing
/// MMIO mappings are therefore visible (as supervisor-only) in the new
/// address space with no further work, since those mappings are all
/// established once at boot and never change afterward. The lower half
/// starts completely empty, ready for a task's own mappings.
pub fn new_address_space() -> AddrSpace {
    let pml4_phys = pmm::alloc_page();
    let pml4 = table(pml4_phys);
    let current = table(current_cr3());
    pml4[..ENTRIES / 2].fill(0);
    pml4[ENTRIES / 2..].copy_from_slice(&current[ENTRIES / 2..]);
    AddrSpace { pml4_phys }
}

/// Maps one page-aligned physical page at `virt` within `aspace`,
/// allocating any missing intermediate tables from the PMM. `flags`
/// should include at least PAGE_PRESENT, plus PAGE_WRITABLE/PAGE_USER/
/// PAGE_NOCACHE as appropriate. Never fails -- if the PMM is out of
/// memory for page tables, that's fatal anyway.
pub fn map(aspace: &AddrSpace, virt: u64, phys: u64, flags: u64) {
    *walk_create(aspace.pml4_phys, virt) = (phys & PAGE_ADDR_MASK) | flags;
    unsafe { asm!("invlpg [{}]", in(reg) virt, options(nostack, preserves_flags)) };
}

/// Loads `aspace` into CR3, making it the active address space.
pub fn switch(aspace: &AddrSpace) {
    unsafe { asm!("mov cr3, {}", in(reg) aspace.pml4_phys, options(nostack, preserves_flags)) };
}

/// Every present leaf in the user half (PML4 entries 0-255), as
/// (virtual address, PTE).
fn user_leaves(pml4_phys: u64, mut f: impl FnMut(u64, u64) -> bool) -> bool {
    let present = |e: &&u64| **e & PAGE_PRESENT != 0;
    for (i4, e4) in table(pml4_phys)[..ENTRIES / 2].iter().enumerate().filter(|(_, e)| present(e)) {
        for (i3, e3) in table(e4 & PAGE_ADDR_MASK).iter().enumerate().filter(|(_, e)| present(e)) {
            for (i2, e2) in table(e3 & PAGE_ADDR_MASK).iter().enumerate().filter(|(_, e)| present(e)) {
                for (i1, e1) in table(e2 & PAGE_ADDR_MASK).iter().enumerate().filter(|(_, e)| present(e)) {
                    let virt = (i4 << 39 | i3 << 30 | i2 << 21 | i1 << 12) as u64;
                    if !f(virt, *e1) {
                        return false;
                    }
                }
            }
        }
    }
    true
}

/// fork() support (M13): copies every present page in `src`'s user half
/// into `dst` -- a full physical copy of each page with the same flags,
/// not copy-on-write. `dst` should already exist (new_address_space())
/// with its higher half populated. False on a mid-clone allocation
/// failure (the partial clone is left as-is -- the caller
/// treats the whole fork as failed and discards `dst`, matching this
/// project's "leaks on the failure path are an accepted simplification"
/// pragmatism).
pub fn clone_user_pages(dst: &AddrSpace, src: &AddrSpace) -> bool {
    user_leaves(src.pml4_phys, |virt, pte| {
        let copy = pmm::alloc_page();
        if copy == 0 {
            return false;
        }
        table(copy).copy_from_slice(table(pte & PAGE_ADDR_MASK));
        map(dst, virt, copy, pte & 0xFFF);
        true
    })
}

/// Counterpart to clone_user_pages(): frees every present page in
/// `aspace`'s user half (leaf pages and every intermediate table), then
/// the PML4 itself, back to the PMM. Only safe once `aspace` is
/// definitely not the active address space and nothing is executing on a
/// kernel stack that assumes it's still mapped -- see exec/process.rs's
/// run_until() (reap time) and exec()'s deferred free.
pub fn free_user_pages(aspace: &AddrSpace) {
    let pml4_phys = aspace.pml4_phys;
    let tables = |phys: u64| table(phys).iter().filter(|e| **e & PAGE_PRESENT != 0).map(|e| e & PAGE_ADDR_MASK);
    for pdpt in table(pml4_phys)[..ENTRIES / 2].iter().filter(|e| **e & PAGE_PRESENT != 0).map(|e| e & PAGE_ADDR_MASK) {
        for pd in tables(pdpt) {
            for pt in tables(pd) {
                tables(pt).for_each(|page| pmm::free_page(page));
                pmm::free_page(pt);
            }
            pmm::free_page(pd);
        }
        pmm::free_page(pdpt);
    }
    pmm::free_page(pml4_phys);
}

/// Maps `size` bytes of physical memory at `phys` (need not be
/// page-aligned) into the kernel's own (currently active) address space
/// as present + writable + uncacheable, returning a pointer to the same
/// byte. Exists because PCI BARs (especially 64-bit ones, which QEMU/OVMF
/// often place far above actual RAM, e.g. 0xc000000000) commonly live
/// outside the range Limine's HHDM covers (only memory in the memory map).
pub fn map_mmio(phys: u64, size: u64) -> *mut u8 {
    let page_phys = phys & !(PAGE_SIZE - 1);
    let pages = (phys + size - page_phys).div_ceil(PAGE_SIZE);
    let next = unsafe { NEXT_MMIO_VIRT.get() };
    let virt = *next;
    *next += pages * PAGE_SIZE;

    let aspace = current_address_space();
    for i in 0..pages {
        map(&aspace, virt + i * PAGE_SIZE, page_phys + i * PAGE_SIZE, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NOCACHE);
    }
    (virt + phys - page_phys) as *mut u8
}
