#include "usermode.h"
#include "gdt.h"
#include "../../mm/vmm.h"

#include <stdint.h>

/* Defined in usermode.S. */
extern void arch_enter_usermode(uint64_t entry, uint64_t user_rsp);
extern void arch_return_to_kernel(int status) __attribute__((noreturn));
extern int kernel_resume_status; /* Written by arch_return_to_kernel before it jumps back. */

void enter_usermode(struct usertask *t, int *exit_status) {
    struct addr_space caller_as = vmm_current_address_space();

    tss_set_kernel_stack(t->kernel_stack_top);
    vmm_switch(&t->as);

    arch_enter_usermode(t->entry, t->user_stack_top);
    /* Execution resumes here -- via arch_return_to_kernel's `ret` -- once
     * the task exits or faults. CR3 is still whatever the task was
     * using; restore the caller's own address space before doing
     * anything else. */

    vmm_switch(&caller_as);
    *exit_status = kernel_resume_status;
}

void return_to_kernel(int status) {
    arch_return_to_kernel(status);
}
