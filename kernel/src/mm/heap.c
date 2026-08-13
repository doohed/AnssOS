#include "heap.h"
#include "pmm.h"
#include "../boot/requests.h"

#include <stddef.h>
#include <stdint.h>

#define HEAP_ALIGN 16

struct block_header {
    struct block_header *next; /* Next block in address order (both free and used). */
    size_t size;               /* Usable size, not including this header. */
    int is_free;
};

static struct block_header *heap_head; /* First block ever, address order. */
static struct block_header *heap_tail; /* Last block -- O(1) append when growing. */

void heap_init(void) {
    heap_head = NULL;
    heap_tail = NULL;
}

static size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

/* Pulls enough whole pages from the PMM to satisfy `min_size` bytes of
 * usable space (plus this block's own header), formats them as one new
 * free block, and appends it to the block list. */
static struct block_header *grow_heap(size_t min_size) {
    size_t needed = sizeof(struct block_header) + min_size;
    uint64_t pages = (needed + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        return NULL;
    }

    struct block_header *block =
        (struct block_header *)(uintptr_t)(phys + hhdm_request.response->offset);
    block->size = pages * PMM_PAGE_SIZE - sizeof(struct block_header);
    block->is_free = 1;
    block->next = NULL;

    if (heap_tail != NULL) {
        heap_tail->next = block;
    } else {
        heap_head = block;
    }
    heap_tail = block;

    return block;
}

/* Carves `size` usable bytes off the front of `block`, turning the
 * remainder into a new free block right after it -- but only if that
 * remainder is big enough to be worth tracking on its own; otherwise the
 * slack just stays attached to this allocation. */
static void split_block(struct block_header *block, size_t size) {
    if (block->size < size + sizeof(struct block_header) + HEAP_ALIGN) {
        return;
    }

    uint8_t *payload = (uint8_t *)(block + 1);
    struct block_header *rest = (struct block_header *)(payload + size);
    rest->size = block->size - size - sizeof(struct block_header);
    rest->is_free = 1;
    rest->next = block->next;

    block->size = size;
    block->next = rest;

    if (heap_tail == block) {
        heap_tail = rest;
    }
}

void *kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    size = align_up(size, HEAP_ALIGN);

    for (struct block_header *b = heap_head; b != NULL; b = b->next) {
        if (b->is_free && b->size >= size) {
            split_block(b, size);
            b->is_free = 0;
            return (void *)(b + 1);
        }
    }

    struct block_header *fresh = grow_heap(size);
    if (fresh == NULL) {
        return NULL;
    }
    split_block(fresh, size);
    fresh->is_free = 0;
    return (void *)(fresh + 1);
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    struct block_header *block = ((struct block_header *)ptr) - 1;
    block->is_free = 1;

    /* Forward coalescing only, and only when the next block in the list */
    /* is truly adjacent in memory -- blocks from different grow_heap() */
    /* calls aren't necessarily contiguous, since pmm_alloc_pages() just */
    /* returns *a* free run, not one adjacent to the last allocation. */
    if (block->next != NULL && block->next->is_free) {
        uint8_t *end_of_block = (uint8_t *)(block + 1) + block->size;
        if ((uint8_t *)block->next == end_of_block) {
            struct block_header *next = block->next;
            block->size += sizeof(struct block_header) + next->size;
            block->next = next->next;
            if (heap_tail == next) {
                heap_tail = block;
            }
        }
    }
}
