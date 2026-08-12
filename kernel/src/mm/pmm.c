#include "pmm.h"
#include "../boot/requests.h"
#include "../drivers/serial.h"
#include "../lib/string.h"

#include <stdint.h>

static uint8_t *bitmap;
static uint64_t bitmap_size_bytes;
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t alloc_cursor;

static inline void bitmap_set(uint64_t page) {
    bitmap[page / 8] |= (uint8_t)(1u << (page % 8));
}

static inline void bitmap_clear(uint64_t page) {
    bitmap[page / 8] &= (uint8_t)~(1u << (page % 8));
}

static inline int bitmap_test(uint64_t page) {
    return bitmap[page / 8] & (uint8_t)(1u << (page % 8));
}

void pmm_init(void) {
    if (memmap_request.response == NULL || hhdm_request.response == NULL) {
        kprintf("PMM: PANIC: missing memmap or HHDM response from bootloader\n");
        for (;;) {
            asm volatile ("cli; hlt");
        }
    }

    struct limine_memmap_response *mm = memmap_request.response;
    uint64_t hhdm_offset = hhdm_request.response->offset;

    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE ||
            e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            uint64_t end = e->base + e->length;
            if (end > highest_addr) {
                highest_addr = end;
            }
        }
    }

    total_pages = highest_addr / PMM_PAGE_SIZE;
    bitmap_size_bytes = (total_pages + 7) / 8;

    /* Park the bitmap itself in the first usable region big enough to */
    /* hold it, and reach it through the HHDM until we have our own */
    /* virtual memory manager. */
    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_size_bytes) {
            bitmap_phys = e->base;
            break;
        }
    }

    if (bitmap_phys == 0) {
        kprintf("PMM: PANIC: no usable region large enough for the page bitmap\n");
        for (;;) {
            asm volatile ("cli; hlt");
        }
    }

    bitmap = (uint8_t *)(bitmap_phys + hhdm_offset);
    memset(bitmap, 0xFF, bitmap_size_bytes); /* Start with everything used. */

    free_pages = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }
        uint64_t start_page = e->base / PMM_PAGE_SIZE;
        uint64_t page_count = e->length / PMM_PAGE_SIZE;
        for (uint64_t p = 0; p < page_count; p++) {
            bitmap_clear(start_page + p);
            free_pages++;
        }
    }

    /* The region we just marked free includes the pages the bitmap lives */
    /* in -- claw those back. */
    uint64_t bitmap_pages = (bitmap_size_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t bitmap_start_page = bitmap_phys / PMM_PAGE_SIZE;
    for (uint64_t p = 0; p < bitmap_pages; p++) {
        if (!bitmap_test(bitmap_start_page + p)) {
            bitmap_set(bitmap_start_page + p);
            free_pages--;
        }
    }

    alloc_cursor = 0;

    kprintf("PMM: %lu pages tracked, %lu MiB free\n",
            total_pages, (free_pages * PMM_PAGE_SIZE) / (1024 * 1024));
}

static uint64_t try_alloc_from(uint64_t start, uint64_t end) {
    for (uint64_t i = start; i < end; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
            alloc_cursor = i + 1;
            return i * PMM_PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_page(void) {
    uint64_t addr = try_alloc_from(alloc_cursor, total_pages);
    if (addr != 0 || alloc_cursor == 0) {
        return addr;
    }
    /* Wrap around: pages before the cursor may have been freed since. */
    return try_alloc_from(0, alloc_cursor);
}

void pmm_free_page(uint64_t phys_addr) {
    uint64_t page = phys_addr / PMM_PAGE_SIZE;
    if (page >= total_pages || !bitmap_test(page)) {
        return; /* Out of range or double free -- ignore. */
    }
    bitmap_clear(page);
    free_pages++;
}

uint64_t pmm_alloc_pages(uint64_t count) {
    if (count == 0 || count > total_pages) {
        return 0;
    }

    uint64_t run = 0;
    for (uint64_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i)) {
            run = 0;
            continue;
        }
        run++;
        if (run == count) {
            uint64_t start = i - count + 1;
            for (uint64_t p = start; p <= i; p++) {
                bitmap_set(p);
            }
            free_pages -= count;
            return start * PMM_PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_free_page_count(void) {
    return free_pages;
}

uint64_t pmm_total_page_count(void) {
    return total_pages;
}
