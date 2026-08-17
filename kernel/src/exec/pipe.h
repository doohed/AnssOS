#ifndef EXEC_PIPE_H
#define EXEC_PIPE_H

#include <stddef.h>
#include <stdint.h>

#define PIPE_BUF_SIZE 4096

/* A unidirectional in-kernel byte ring buffer -- the transport under
 * pipe()/use_as_stdio() (see exec/syscall.c). Deliberately non-blocking
 * only: irq_handler()'s preemption check (arch/x86_64/idt.c) only fires
 * when the timer interrupts *ring-3* code, never ring-0, so a busy-spin
 * inside a syscall handler waiting on another process would never yield
 * the CPU to that process -- a real deadlock, not a style issue. Every
 * operation here returns immediately; callers that want blocking
 * semantics loop in their own ring-3 code instead (safely preemptible),
 * the same shape play.c's poll_key() loop already uses. */
struct pipe {
    uint8_t buf[PIPE_BUF_SIZE];
    size_t head;  /* Index of the next byte to read. */
    size_t count; /* Bytes currently buffered. */

    /* Reference counts, not booleans -- fork() shallow-copies
     * open_files[] (exec/process.c's process_fork(), "sharing the same
     * vnode pointers" -- true of pipe pointers too), so after a fork()
     * both parent and child hold independent table entries referencing
     * the *same* pipe. A plain "is this end open" boolean would let one
     * process's close() of its own copy incorrectly close the end out
     * from under a sibling still using it -- found exactly this way, via
     * userland/pipetest.c's exec()'d-child case silently capturing zero
     * bytes (the child's own close() of its unneeded read-end copy
     * closed the *shared* object's read end before the exec'd program
     * ever got to write). pipe_create() sets both to 1 (the two table
     * entries pipe() itself just made); process_fork() must increment
     * the relevant count for every pipe-backed entry it copies -- see
     * its own comment. */
    int read_refs;
    int write_refs;
};

struct pipe *pipe_create(void);

/* Copies up to `len` bytes in; if the ring doesn't have room for all of
 * it, copies as much as fits (a short write, never blocks). Returns the
 * byte count copied, or -1 if there are no read-end references left. */
int pipe_write(struct pipe *p, const void *buf, size_t len);

/* Copies up to `len` bytes out. Returns >0 (bytes copied) if any were
 * buffered, 0 if empty but a write-end reference still exists (try
 * again later), or -1 if empty and every write-end reference is gone
 * (EOF). */
int pipe_read(struct pipe *p, void *buf, size_t len);

/* Drops one reference to the respective end; frees `p` once *all*
 * references to *both* ends are gone. Call once per pipe-backed
 * open_files[] entry (or stdin_pipe/stdout_pipe) that stops referencing
 * this pipe, whether via close(), process exit, or exec() abandoning a
 * table (it doesn't -- see process_exec()'s save/restore). */
void pipe_close_read(struct pipe *p);
void pipe_close_write(struct pipe *p);

#endif
