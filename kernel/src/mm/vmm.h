#ifndef MM_VMM_H
#define MM_VMM_H

#include <stdint.h>

/* Virtual memory support: a real 4-level page-table walker, used for two
 * things so far -- mapping arbitrary physical MMIO ranges (PCI BARs) into
 * the kernel's own address space, and (M10) building separate,
 * per-user-task address spaces for ring-3 execution. Everything else
 * still rides on Limine's HHDM. There's no unmapping and no demand
 * paging here. */

#define PAGE_PRESENT (1ull << 0)
#define PAGE_WRITABLE (1ull << 1)
#define PAGE_USER (1ull << 2)    /* Ring-3-accessible; see vmm_map(). */
#define PAGE_NOCACHE (1ull << 4) /* PCD -- required for correct MMIO semantics. */

/* A page-table hierarchy, identified by its top-level (PML4) physical
 * address -- what CR3 holds while that address space is active. */
struct addr_space {
    uint64_t pml4_phys;
};

/* The address space active at boot and used by the kernel itself
 * (read from CR3) -- every fresh address space created by
 * vmm_new_address_space() shares this one's higher half (kernel code,
 * HHDM, MMIO) automatically. */
struct addr_space vmm_current_address_space(void);

/* Allocates a fresh top-level page table, zeroes it, then copies PML4
 * entries 256-511 (the whole canonical higher half) from the currently
 * active address space -- kernel code/data, the HHDM, and any existing
 * MMIO mappings are therefore visible (as supervisor-only) in the new
 * address space with no further work, since those mappings are all
 * established once at boot and never change afterward. The lower half
 * (256 entries) starts completely empty, ready for a task's own
 * mappings via vmm_map(). */
struct addr_space vmm_new_address_space(void);

/* Maps one page-aligned physical page at `virt` within `as`, walking/
 * extending the hierarchy (allocating any missing intermediate levels
 * via the PMM, always present+writable+user so the U/S bit is never the
 * reason a leaf mapping's own `flags` is unreachable -- see the .c file).
 * `flags` should include at least PAGE_PRESENT, plus PAGE_WRITABLE/
 * PAGE_USER/PAGE_NOCACHE as appropriate for the mapping. Never fails --
 * if the PMM is out of memory for page tables, that's fatal anyway. */
void vmm_map(struct addr_space *as, uint64_t virt, uint64_t phys, uint64_t flags);

/* Loads `as` into CR3, making it the active address space. */
void vmm_switch(struct addr_space *as);

/* Translates a physical address to its HHDM-mapped kernel virtual
 * address -- the same "identity map plus offset" pmm.c/heap.c already
 * use internally to touch freshly allocated pages. Exposed for callers
 * (exec/elf.c, exec/syscall.c) that need to write into memory obtained
 * from pmm_alloc_page()/pmm_alloc_pages() before/instead of mapping it
 * into a user address space via vmm_map(). */
void *vmm_phys_to_virt(uint64_t phys);

/* Maps `size` bytes of physical memory starting at `phys_addr` (need not
 * be page-aligned) into the kernel's own (currently active) address
 * space as present + writable + uncacheable. Returns a virtual pointer
 * to the same byte offset within the mapping. Exists because PCI BARs
 * (especially 64-bit ones, which QEMU/OVMF often place far above actual
 * RAM, e.g. 0xc000000000) commonly live outside the range Limine's HHDM
 * covers (HHDM only maps memory described in the memory map). */
volatile void *vmm_map_mmio(uint64_t phys_addr, uint64_t size);

#endif
