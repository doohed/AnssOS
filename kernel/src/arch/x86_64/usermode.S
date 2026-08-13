/* The actual ring-0 <-> ring-3 transition, hand-written since it needs to
 * build a raw iretq frame and do a bespoke one-shot context switch that
 * C can't express. See usermode.c for the higher-level API this backs. */

/* Selectors, matching gdt.c's struct gdt_table layout (see gdt.h) --
 * RPL=3 baked in, since these are only ever loaded while entering ring 3. */
#define SEL_UCODE 0x1B /* 0x18 | 3 */
#define SEL_UDATA 0x23 /* 0x20 | 3 */

.text

/* void arch_enter_usermode(uint64_t entry, uint64_t user_rsp);
 * (SysV: entry in rdi, user_rsp in rsi.)
 *
 * Saves the current (kernel) callee-saved registers and rsp -- a
 * hand-rolled setjmp -- then builds an iretq frame and jumps into ring 3.
 * Never returns directly to its caller; the only way back is
 * arch_return_to_kernel's `ret`, landing exactly at this call's return
 * address as if arch_enter_usermode had returned normally. */
.global arch_enter_usermode
arch_enter_usermode:
    mov %rbx, kernel_resume_regs+0(%rip)
    mov %rbp, kernel_resume_regs+8(%rip)
    mov %r12, kernel_resume_regs+16(%rip)
    mov %r13, kernel_resume_regs+24(%rip)
    mov %r14, kernel_resume_regs+32(%rip)
    mov %r15, kernel_resume_regs+40(%rip)
    mov %rsp, kernel_resume_regs+48(%rip) /* points at our own return address right now */

    /* iretq pops, in order: rip, cs, rflags, rsp, ss -- so push in the
     * reverse order. */
    pushq $SEL_UDATA
    pushq %rsi           /* user_rsp */
    pushq $0x202         /* rflags: reserved bit 1 set, IF=1 */
    pushq $SEL_UCODE
    pushq %rdi           /* entry */
    iretq

/* void arch_return_to_kernel(int status);
 *
 * Restores the context arch_enter_usermode saved and resumes it via
 * `ret` -- from the perspective of whoever called arch_enter_usermode,
 * it just returned. Never returns to its own caller. */
.global arch_return_to_kernel
arch_return_to_kernel:
    mov %edi, kernel_resume_status(%rip)
    mov kernel_resume_regs+0(%rip), %rbx
    mov kernel_resume_regs+8(%rip), %rbp
    mov kernel_resume_regs+16(%rip), %r12
    mov kernel_resume_regs+24(%rip), %r13
    mov kernel_resume_regs+32(%rip), %r14
    mov kernel_resume_regs+40(%rip), %r15
    mov kernel_resume_regs+48(%rip), %rsp
    ret

.section .bss
.align 8
kernel_resume_regs: /* rbx, rbp, r12, r13, r14, r15, rsp -- 7 quads */
    .skip 8*7
.global kernel_resume_status
kernel_resume_status:
    .skip 4

.section .note.GNU-stack,"",@progbits
