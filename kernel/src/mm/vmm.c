#include "vmm.h"
#include "pmm.h"
#include "../boot/requests.h"
#include "../lib/string.h"

#include <stdint.h>

#define PAGE_PRESENT (1ull << 0)
#define PAGE_WRITABLE (1ull << 1)
#define PAGE_NOCACHE (1ull << 4) /* PCD -- required for correct MMIO semantics. */
#define PAGE_ADDR_MASK 0x000ffffffffff000ull

#define PAGE_TABLE_ENTRIES 512

/* Bump allocator for MMIO virtual addresses: a dedicated slice of the */
/* higher half, comfortably below the kernel image (0xffffffff80000000) */
/* and far above the HHDM (which starts at hhdm_offset, covering only */
/* actual RAM). Mappings are never reused/freed. */
static uint64_t next_mmio_virt = 0xffffff8000000000ull;

static inline uint64_t *table_virt(uint64_t phys) {
    return (uint64_t *)(uintptr_t)(phys + hhdm_request.response->offset);
}

static uint64_t *get_or_create_table(uint64_t *parent, uint64_t index) {
    if (!(parent[index] & PAGE_PRESENT)) {
        uint64_t new_table_phys = pmm_alloc_page();
        memset(table_virt(new_table_phys), 0, PMM_PAGE_SIZE);
        parent[index] = new_table_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }
    return table_virt(parent[index] & PAGE_ADDR_MASK);
}

volatile void *vmm_map_mmio(uint64_t phys_addr, uint64_t size) {
    uint64_t page_phys = phys_addr & ~(uint64_t)(PMM_PAGE_SIZE - 1);
    uint64_t page_offset = phys_addr - page_phys;
    uint64_t end = phys_addr + size;
    uint64_t page_count = (end - page_phys + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    uint64_t virt_base = next_mmio_virt;
    next_mmio_virt += page_count * PMM_PAGE_SIZE;

    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = table_virt(cr3 & PAGE_ADDR_MASK);

    for (uint64_t i = 0; i < page_count; i++) {
        uint64_t virt = virt_base + i * PMM_PAGE_SIZE;
        uint64_t phys = page_phys + i * PMM_PAGE_SIZE;

        uint64_t pml4_idx = (virt >> 39) & 0x1FF;
        uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
        uint64_t pd_idx = (virt >> 21) & 0x1FF;
        uint64_t pt_idx = (virt >> 12) & 0x1FF;

        uint64_t *pdpt = get_or_create_table(pml4, pml4_idx);
        uint64_t *pd = get_or_create_table(pdpt, pdpt_idx);
        uint64_t *pt = get_or_create_table(pd, pd_idx);

        pt[pt_idx] = phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_NOCACHE;
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }

    return (volatile void *)(uintptr_t)(virt_base + page_offset);
}
