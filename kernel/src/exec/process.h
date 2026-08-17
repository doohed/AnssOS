#ifndef EXEC_PROCESS_H
#define EXEC_PROCESS_H

#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/usermode.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_PROCESSES 16

/* A process spawned directly by the shell (not by another process'
 * fork()) has this as its parent_pid -- there's no init/kernel "process"
 * to report to, just a sentinel meaning "nobody will wait() for me but
 * the top-level scheduler_run_until(-1) loop that launched me." */
#define KERNEL_PARENT_PID (-1)

enum process_state {
    PROC_UNUSED,
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_ZOMBIE,
};

struct process {
    int pid;
    int parent_pid;
    enum process_state state;
    int exit_status;

    /* Whether this process has ever been dispatched before: false means
     * its next dispatch should be a fresh launch at task.entry (see
     * enter_usermode()); true means it should resume from saved_rsp
     * (see resume_usermode()) -- true for a fork()'d child from the
     * moment it's created (see process_fork()), and for any process
     * once a real preemption has happened to it. */
    int has_run;
    uint64_t saved_rsp;

    struct usertask task;

    /* A previous exec() (see process_exec()) left this process's *old*
     * address space/kernel stack behind to free later -- not safe to
     * free at the moment they're replaced, since process_exec() is
     * still executing on the old kernel stack when it runs. Freed by
     * process_free_pending() at the start of this process's next
     * dispatch (see scheduler_run_until()), once we're definitely off
     * both. pending_free_as.pml4_phys == 0 / pending_free_kstack == 0
     * mean "nothing pending." */
    struct addr_space pending_free_as;
    uint64_t pending_free_kstack;
};

/* Parses `image` (a static non-PIE ELF64, see exec/elf.c's elf_load(),
 * which this calls directly) into a brand new process-table slot with
 * `parent_pid` as its parent -- KERNEL_PARENT_PID for a top-level
 * process launched directly by the shell. Returns the new pid, or -1 on
 * failure (reports why via kprintf, same as elf_load()). */
int process_spawn(const uint8_t *image, size_t image_size, int argc, const char *const *argv,
                  struct vnode *cwd, int parent_pid);

/* fork(): duplicates `parent`'s address space (full page copy, no COW --
 * see vmm_clone_user_pages(), mm/vmm.h), heap range, open files, and cwd
 * into a brand new process-table slot, PROC_RUNNABLE, with an initial
 * saved frame that's an exact copy of `frame` (the parent's own syscall
 * frame) except rax=0 -- so the child's very first resume "returns" 0
 * from fork(), landing right after the same int $0x80 the parent used,
 * with its own copy of every register/the user stack at that point.
 * Returns the new child's pid (the parent's own fork() return value,
 * written by the caller into `frame->rax` normally, since the parent's
 * own syscall handling continues straight through), or -1 on failure. */
int process_fork(struct process *parent, struct interrupt_frame *frame);

/* exec(): replaces `p`'s own address space/entry/heap/stack in place
 * with `image` -- same pid, same open files/cwd, parent unchanged.
 * Resets has_run to 0 so the next dispatch is a fresh launch at the new
 * entry point. Returns -1 on failure (the caller's old image is left
 * running, exactly as if exec() had simply returned -1 like any other
 * failed syscall); on success there's nothing more to report -- the
 * caller finds out only by actually running the new program. */
int process_exec(struct process *p, const uint8_t *image, size_t image_size, int argc,
                 const char *const *argv);

/* Closes `p`'s pipe-backed stdin/stdout (M19, exec/pipe.h), if any --
 * called from both places a process becomes PROC_ZOMBIE (exec/syscall.c's
 * SYS_exit, and idt.c's non-fatal user-fault path) so a pane's shell
 * exiting or crashing promptly shows up as EOF to whoever's reading its
 * output pipe, rather than that pipe just silently never producing
 * anything more. */
void process_close_stdio_pipes(struct process *p);

struct process *process_by_pid(int pid);

/* Finds a process that's `parent_pid`'s child, matching `pid` exactly if
 * `pid >= 0`, or any child at all if `pid < 0` -- preferring an already-
 * PROC_ZOMBIE one over a still-running one when `pid < 0` and more than
 * one child qualifies, so a wait()-for-any call completes immediately if
 * any child has already exited. NULL means "no such/any child at all"
 * (wait()'s own error case) -- not the same as "found one, but it's
 * still running," which the caller distinguishes via the returned
 * process's own state. */
struct process *process_find_child(int parent_pid, int pid);

/* The process currently "inside" a dispatch (enter_usermode()/
 * resume_usermode()) somewhere in the current call chain -- NULL if
 * none is. Unlike usermode_current_task(), this can be meaningfully
 * "stale" for a brief window while a nested scheduler_run_until() call
 * (see below) is unwinding back to an outer one; nothing relies on it
 * during that window. */
struct process *process_current(void);

/* Runs processes (round-robin among PROC_RUNNABLE ones) until:
 *   - `wait_pid >= 0`: that pid becomes PROC_ZOMBIE, which is then
 *     reaped and its exit status returned. Called both by the wait()/
 *     waitpid() syscall (as an ordinary, *recursive* C call from deep
 *     inside another process' own syscall handling -- see syscall.c's
 *     sys_wait_impl()) and, indirectly, by process_exec()'s caller.
 *   - `wait_pid < 0`: every process-table slot is empty. Any zombie
 *     encountered along the way is reaped immediately (this project has
 *     no init process to reparent orphans to -- the convention is that
 *     a process which fork()s a child waits for it before exiting, so
 *     the only zombie a -1 caller should ever actually see is its own
 *     top-level launch). Returns 0. Used by the shell's `run` (see
 *     shell.c) to block until an entire process family has finished. */
int scheduler_run_until(int wait_pid);

#endif
