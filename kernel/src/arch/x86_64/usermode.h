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
 * exec/syscall.c), and its open file table. */
struct usertask {
    uint64_t entry;
    uint64_t user_stack_top;
    uint64_t kernel_stack_top;
    struct addr_space as;
    uint64_t heap_start;
    uint64_t heap_end;
    struct open_file open_files[MAX_OPEN_FILES];
};

/* The task currently "inside" enter_usermode() (see below), or NULL if
 * none is -- lets syscall_dispatch() (exec/syscall.c) reach the running
 * task's heap/fd state. Valid only while a task is actually running;
 * only ever one at a time, per enter_usermode()'s own restriction. */
struct usertask *usermode_current_task(void);

/* Switches into `t`'s address space and jumps into ring 3 at `t->entry`
 * with `t->user_stack_top` as the user stack. Blocks (from the caller's
 * point of view) until the task calls exit() or takes a fault -- see
 * return_to_kernel() below -- at which point execution resumes right
 * here as if this had simply returned, with `*exit_status` set to
 * whatever status the task exited/faulted with. Restores the caller's
 * own address space before returning. Only one task may be "inside"
 * enter_usermode() at a time -- there's no support yet for a user task
 * itself calling enter_usermode() again (no nested ring-3 execution). */
void enter_usermode(struct usertask *t, int *exit_status);

/* Abandons whichever ring-3 context is currently running (there is
 * always exactly one, while inside enter_usermode()) and resumes the
 * kernel context that called enter_usermode(), as if that call had just
 * returned, with `status` as its exit status. Called from the exit
 * syscall and from the non-fatal user-mode-fault path in idt.c. Never
 * returns to its own caller. */
void return_to_kernel(int status) __attribute__((noreturn));

#endif
