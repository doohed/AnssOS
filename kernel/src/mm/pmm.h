#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdint.h>

#define PMM_PAGE_SIZE 4096

/* Bitmap physical page allocator, built from Limine's memory map. Must be */
/* called after the HHDM and memmap Limine requests have valid responses */
/* (see boot/requests.h) since it needs both to place and address the */
/* bitmap itself. */
void pmm_init(void);

/* Returns a physical address, or 0 on out-of-memory. 0 is never a valid */
/* allocation since physical page 0 is always reserved. */
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys_addr);

/* Allocates `count` physically contiguous pages (e.g. for a DMA */
/* framebuffer that needs to be one linear range). Returns 0 on failure. */
/* There's no pmm_free_pages() yet -- nothing in the current milestones */
/* frees a contiguous range once allocated. */
uint64_t pmm_alloc_pages(uint64_t count);

uint64_t pmm_free_page_count(void);
uint64_t pmm_total_page_count(void);

#endif
