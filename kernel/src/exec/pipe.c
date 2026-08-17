#include "pipe.h"
#include "../mm/heap.h"

#include <stddef.h>

struct pipe *pipe_create(void) {
    struct pipe *p = kmalloc(sizeof(struct pipe));
    if (p == NULL) {
        return NULL;
    }
    p->head = 0;
    p->count = 0;
    p->read_refs = 1;
    p->write_refs = 1;
    return p;
}

int pipe_write(struct pipe *p, const void *buf, size_t len) {
    if (p->read_refs == 0) {
        return -1;
    }
    const uint8_t *src = buf;
    size_t space = PIPE_BUF_SIZE - p->count;
    size_t n = len < space ? len : space;
    size_t tail = (p->head + p->count) % PIPE_BUF_SIZE;
    for (size_t i = 0; i < n; i++) {
        p->buf[(tail + i) % PIPE_BUF_SIZE] = src[i];
    }
    p->count += n;
    return (int)n;
}

int pipe_read(struct pipe *p, void *buf, size_t len) {
    if (p->count == 0) {
        return p->write_refs > 0 ? 0 : -1;
    }
    uint8_t *dst = buf;
    size_t n = len < p->count ? len : p->count;
    for (size_t i = 0; i < n; i++) {
        dst[i] = p->buf[(p->head + i) % PIPE_BUF_SIZE];
    }
    p->head = (p->head + n) % PIPE_BUF_SIZE;
    p->count -= n;
    return (int)n;
}

void pipe_close_read(struct pipe *p) {
    p->read_refs--;
    if (p->read_refs == 0 && p->write_refs == 0) {
        kfree(p);
    }
}

void pipe_close_write(struct pipe *p) {
    p->write_refs--;
    if (p->read_refs == 0 && p->write_refs == 0) {
        kfree(p);
    }
}
