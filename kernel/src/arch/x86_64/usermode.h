#ifndef ARCH_X86_64_USERMODE_H
#define ARCH_X86_64_USERMODE_H

#include "../../mm/vmm.h"

#include <stdint.h>

/* Everything needed to run one program in ring 3: its entry point, its
 * own address space, and the two stacks it needs -- a user stack (for
 * its own code) and a kernel stack (TSS.RSP0, used only while the CPU is
 * inside a syscall/exception taken on this task's behalf). */
struct usertask {
    uint64_t entry;
    uint64_t user_stack_top;
    uint64_t kernel_stack_top;
    struct addr_space as;
};

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
