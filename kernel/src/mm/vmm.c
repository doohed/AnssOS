#include "vmm.h"
#include "pmm.h"
#include "../boot/requests.h"
#include "../lib/string.h"

#include <stdint.h>

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

static inline uint64_t current_cr3(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & PAGE_ADDR_MASK;
}

/* Intermediate (non-leaf) table entries are always present+writable+user,
 * regardless of what the eventual leaf mapping needs: the CPU ANDs the
 * U/S bit across every level of the walk, so an intermediate entry
 * without it would make a user-accessible leaf unreachable from ring 3.
 * This doesn't weaken protection -- what's actually reachable is still
 * gated by the leaf PTE's own flags, which vmm_map()'s caller controls;
 * a purely-kernel subtree simply never gets a leaf with PAGE_USER set. */
static uint64_t *get_or_create_table(uint64_t *parent, uint64_t index) {
    if (!(parent[index] & PAGE_PRESENT)) {
        uint64_t new_table_phys = pmm_alloc_page();
        memset(table_virt(new_table_phys), 0, PMM_PAGE_SIZE);
        parent[index] = new_table_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }
    return table_virt(parent[index] & PAGE_ADDR_MASK);
}

static uint64_t *walk_create(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pml4 = table_virt(pml4_phys);
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t pt_idx = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_create_table(pml4, pml4_idx);
    uint64_t *pd = get_or_create_table(pdpt, pdpt_idx);
    uint64_t *pt = get_or_create_table(pd, pd_idx);
    return &pt[pt_idx];
}

struct addr_space vmm_current_address_space(void) {
    return (struct addr_space){.pml4_phys = current_cr3()};
}

struct addr_space vmm_new_address_space(void) {
    uint64_t pml4_phys = pmm_alloc_page();
    uint64_t *pml4 = table_virt(pml4_phys);
    memset(pml4, 0, PMM_PAGE_SIZE);

    uint64_t *current_pml4 = table_virt(current_cr3());
    for (uint64_t i = PAGE_TABLE_ENTRIES / 2; i < PAGE_TABLE_ENTRIES; i++) {
        pml4[i] = current_pml4[i];
    }

    return (struct addr_space){.pml4_phys = pml4_phys};
}

void vmm_map(struct addr_space *as, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pte = walk_create(as->pml4_phys, virt);
    *pte = (phys & PAGE_ADDR_MASK) | flags;
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_switch(struct addr_space *as) {
    asm volatile("mov %0, %%cr3" : : "r"(as->pml4_phys) : "memory");
}

void *vmm_phys_to_virt(uint64_t phys) {
    return table_virt(phys);
}

int vmm_clone_user_pages(struct addr_space *dst, struct addr_space *src) {
    uint64_t *src_pml4 = table_virt(src->pml4_phys);

    for (uint64_t i4 = 0; i4 < PAGE_TABLE_ENTRIES / 2; i4++) {
        if (!(src_pml4[i4] & PAGE_PRESENT)) {
            continue;
        }
        uint64_t *src_pdpt = table_virt(src_pml4[i4] & PAGE_ADDR_MASK);

        for (uint64_t i3 = 0; i3 < PAGE_TABLE_ENTRIES; i3++) {
            if (!(src_pdpt[i3] & PAGE_PRESENT)) {
                continue;
            }
            uint64_t *src_pd = table_virt(src_pdpt[i3] & PAGE_ADDR_MASK);

            for (uint64_t i2 = 0; i2 < PAGE_TABLE_ENTRIES; i2++) {
                if (!(src_pd[i2] & PAGE_PRESENT)) {
                    continue;
                }
                uint64_t *src_pt = table_virt(src_pd[i2] & PAGE_ADDR_MASK);

                for (uint64_t i1 = 0; i1 < PAGE_TABLE_ENTRIES; i1++) {
                    if (!(src_pt[i1] & PAGE_PRESENT)) {
                        continue;
                    }

                    uint64_t src_phys = src_pt[i1] & PAGE_ADDR_MASK;
                    uint64_t flags = src_pt[i1] & 0xFFFull;

                    uint64_t dst_phys = pmm_alloc_page();
                    if (dst_phys == 0) {
                        return -1;
                    }
                    memcpy(table_virt(dst_phys), table_virt(src_phys), PMM_PAGE_SIZE);

                    uint64_t virt = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    vmm_map(dst, virt, dst_phys, flags);
                }
            }
        }
    }

    return 0;
}

volatile void *vmm_map_mmio(uint64_t phys_addr, uint64_t size) {
    uint64_t page_phys = phys_addr & ~(uint64_t)(PMM_PAGE_SIZE - 1);
    uint64_t page_offset = phys_addr - page_phys;
    uint64_t end = phys_addr + size;
    uint64_t page_count = (end - page_phys + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    uint64_t virt_base = next_mmio_virt;
    next_mmio_virt += page_count * PMM_PAGE_SIZE;

    struct addr_space as = vmm_current_address_space();
    for (uint64_t i = 0; i < page_count; i++) {
        uint64_t virt = virt_base + i * PMM_PAGE_SIZE;
        uint64_t phys = page_phys + i * PMM_PAGE_SIZE;
        vmm_map(&as, virt, phys, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NOCACHE);
    }

    return (volatile void *)(uintptr_t)(virt_base + page_offset);
}
