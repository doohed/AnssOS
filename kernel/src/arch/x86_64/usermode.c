#include "usermode.h"
#include "gdt.h"
#include "../../mm/vmm.h"

#include <stdint.h>

/* Defined in usermode.S. */
extern void arch_enter_usermode(uint64_t entry, uint64_t user_rsp, uint64_t *save_area);
extern void arch_resume_process(uint64_t saved_rsp, uint64_t *save_area);
extern void arch_return_to_kernel(int status, uint64_t *save_area) __attribute__((noreturn));
extern int kernel_resume_status; /* Written by arch_return_to_kernel before it jumps back. */

static struct usertask *current_task;

struct usertask *usermode_current_task(void) {
    return current_task;
}

/* Shared by enter_usermode()/resume_usermode() -- both are "switch into
 * `t`'s address space and dispatch it one way or another, block until
 * it exits/blocks/is preempted." */
static void dispatch(struct usertask *t, int fresh, uint64_t saved_rsp, int *exit_status) {
    struct addr_space caller_as = vmm_current_address_space();
    struct usertask *outer_task = current_task; /* M13: may itself be non-NULL -- see below. */

    tss_set_kernel_stack(t->kernel_stack_top);
    vmm_switch(&t->as);
    current_task = t;

    if (fresh) {
        arch_enter_usermode(t->entry, t->user_stack_top, t->kernel_resume);
    } else {
        arch_resume_process(saved_rsp, t->kernel_resume);
    }
    /* Execution resumes here -- via arch_return_to_kernel's `ret` -- once
     * the task exits, blocks, or (M13) is preempted. CR3 is still
     * whatever the task was using; restore the caller's own address
     * space before doing anything else. */

    /* Restore, not just NULL: a task can call wait() (see exec/
     * process.c's scheduler_run_until()), which dispatches a *different*
     * task via an ordinary recursive C call while this one's own dispatch
     * is still on the C stack -- when that nested dispatch returns here,
     * `current_task` must go back to reflecting *this* one, not NULL,
     * since this task hasn't actually exited/blocked itself. */
    current_task = outer_task;
    vmm_switch(&caller_as);
    *exit_status = kernel_resume_status;
}

void enter_usermode(struct usertask *t, int *exit_status) {
    dispatch(t, 1, 0, exit_status);
}

void resume_usermode(struct usertask *t, uint64_t saved_rsp, int *exit_status) {
    dispatch(t, 0, saved_rsp, exit_status);
}

void return_to_kernel(int status) {
    struct usertask *t = current_task;
    arch_return_to_kernel(status, t->kernel_resume);
}
