#ifndef ARCH_X86_64_USERMODE_H
#define ARCH_X86_64_USERMODE_H

#include "../../fs/vfs.h"
#include "../../mm/vmm.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_OPEN_FILES 8

/* One of a task's open (non-stdio) file descriptors -- fd = 3 + index
 * into usertask.open_files[]. `vnode == NULL` marks a free slot. */
struct open_file {
    struct vnode *vnode;
    size_t offset;
    int writable;
};

/* Everything needed to run one program in ring 3: its entry point, its
 * own address space, the two stacks it needs (a user stack for its own
 * code, and a kernel stack -- TSS.RSP0, used only while the CPU is
 * inside a syscall/exception taken on this task's behalf), its brk-
 * managed heap region (see exec/elf.c and the SYS_brk handler in
 * exec/syscall.c), its open file table, and its own working directory
 * (always starts at `/` -- no inheritance from the shell that launched
 * it, see exec/elf.c). */
struct usertask {
    uint64_t entry;
    uint64_t user_stack_top;
    uint64_t kernel_stack_top;
    struct addr_space as;
    uint64_t heap_start;
    uint64_t heap_end;
    struct open_file open_files[MAX_OPEN_FILES];
    struct vnode *cwd;

    /* Where arch_enter_usermode()/arch_resume_process() (usermode.S)
     * save this *specific* task's kernel-side resume context (callee-
     * saved GPRs + rsp) -- one slot per task, not a single shared one,
     * since M13's wait()/waitpid() dispatches a *different* task via an
     * ordinary recursive C call while this one is still "in progress"
     * (blocked deep in its own syscall handling) -- a shared slot would
     * get clobbered by that nested dispatch, stranding this task's real
     * resume point. rbx, rbp, r12, r13, r14, r15, rsp, in that order. */
    uint64_t kernel_resume[7];
};

/* The task currently "inside" enter_usermode()/resume_usermode() at the
 * *innermost* active dispatch (see below) -- NULL if none is. Lets
 * syscall_dispatch() (exec/syscall.c) reach the running task's heap/fd
 * state, and return_to_kernel() find which task's kernel_resume to
 * restore from. dispatch() (usermode.c) saves/restores the previous
 * value around its own call, so this unwinds correctly across nested
 * dispatches (M13's wait()) back to whichever task the C call chain
 * returns to next -- it does not mean "the only task currently
 * mid-flight," just "the one whose context ring-3 is executing in (or
 * whose kernel-side C call most recently returned into) right now." */
struct usertask *usermode_current_task(void);

/* Switches into `t`'s address space and jumps into ring 3 at `t->entry`
 * with `t->user_stack_top` as the user stack. Blocks (from the caller's
 * point of view) until the task calls exit() or takes a fault -- see
 * return_to_kernel() below -- at which point execution resumes right
 * here as if this had simply returned, with `*exit_status` set to
 * whatever status the task exited/faulted with. Restores the caller's
 * own address space (and previous usermode_current_task()) before
 * returning -- safe to call again, for a *different* task, before an
 * earlier call has returned (M13's wait(), recursing into another
 * dispatch while this task's own call is still on the C stack). */
void enter_usermode(struct usertask *t, int *exit_status);

/* Counterpart to enter_usermode() for a process that has already run at
 * least once (M13's process table, see exec/process.h): resumes it
 * exactly where it last left off, from `saved_rsp` -- the address of its
 * own already-populated struct interrupt_frame, captured at whatever
 * point it stopped running (a blocking syscall, fork()'s initial child
 * frame, or a preemption). Same blocking/return semantics as
 * enter_usermode() otherwise. */
void resume_usermode(struct usertask *t, uint64_t saved_rsp, int *exit_status);

/* Abandons whichever ring-3 context is currently running (there is
 * always exactly one, while inside enter_usermode()) and resumes the
 * kernel context that called enter_usermode(), as if that call had just
 * returned, with `status` as its exit status. Called from the exit
 * syscall and from the non-fatal user-mode-fault path in idt.c. Never
 * returns to its own caller. */
void return_to_kernel(int status) __attribute__((noreturn));

#endif
