/* An address-ordered free-list allocator, deliberately mirroring
 * kernel/src/mm/heap.c's design (same block-header shape, same first-fit
 * + forward-only-coalescing behavior) -- just re-implemented from
 * scratch since userland can't call kernel functions, growing the
 * process's own brk-managed heap (see kernel/src/exec/syscall.c's
 * SYS_brk handler) instead of pulling pages from the PMM directly. */

#include "../libc.h"

#define HEAP_ALIGN 16
#define GROW_CHUNK 4096 /* Matches PMM_PAGE_SIZE -- brk() grows a page at a time anyway. */

struct block_header {
    struct block_header *next; /* Next block in address order (both free and used). */
    size_t size;               /* Usable size, not including this header. */
    int is_free;
};

static struct block_header *heap_head; /* First block ever, address order. */
static struct block_header *heap_tail; /* Last block -- O(1) append when growing. */
static unsigned long current_brk;
static int brk_initialized;

static size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

/* Grows the process's heap via brk() by enough whole GROW_CHUNKs to
 * satisfy `min_size` bytes of usable space (plus this block's own
 * header), formats the new region as one free block, and appends it to
 * the block list. */
static struct block_header *grow_heap(size_t min_size) {
    if (!brk_initialized) {
        current_brk = (unsigned long)brk(0); /* brk(0) queries the current break. */
        brk_initialized = 1;
    }

    size_t needed = sizeof(struct block_header) + min_size;
    size_t grow_amount = align_up(needed, GROW_CHUNK);

    unsigned long new_end = current_brk + grow_amount;
    if (brk(new_end) != (long)new_end) {
        return NULL; /* out of memory (see HEAP_MAX_BYTES in syscall.c) */
    }

    struct block_header *block = (struct block_header *)current_brk;
    block->size = grow_amount - sizeof(struct block_header);
    block->is_free = 1;
    block->next = NULL;
    current_brk = new_end;

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

    unsigned char *payload = (unsigned char *)(block + 1);
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

void *malloc(size_t size) {
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

void free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    struct block_header *block = ((struct block_header *)ptr) - 1;
    block->is_free = 1;

    /* Forward coalescing only, and only when the next block in the list
     * is truly adjacent in memory -- blocks from different grow_heap()
     * calls are always contiguous here (brk() only ever extends the
     * break upward from wherever it last was), but kept as an explicit
     * check for the same reason heap.c does: it costs nothing and stays
     * correct if that ever changes. */
    if (block->next != NULL && block->next->is_free) {
        unsigned char *end_of_block = (unsigned char *)(block + 1) + block->size;
        if ((unsigned char *)block->next == end_of_block) {
            struct block_header *next = block->next;
            block->size += sizeof(struct block_header) + next->size;
            block->next = next->next;
            if (heap_tail == next) {
                heap_tail = block;
            }
        }
    }
}
