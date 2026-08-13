#ifndef MM_HEAP_H
#define MM_HEAP_H

#include <stddef.h>

/* Simple first-fit heap allocator on top of the PMM -- kmalloc/kfree for
 * variable-sized allocations (VFS directory/file nodes, file contents),
 * which pmm_alloc_page()/pmm_alloc_pages() alone can't provide (page
 * granularity only). Grows on demand by pulling whole pages from the PMM.
 * No backward coalescing on free -- a documented fragmentation
 * trade-off, not a correctness issue. */

void heap_init(void);

void *kmalloc(size_t size);
void kfree(void *ptr);

#endif
