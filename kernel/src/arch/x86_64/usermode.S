/* The actual ring-0 <-> ring-3 transition, hand-written since it needs to
 * build a raw iretq frame and do a bespoke one-shot context switch that
 * C can't express. See usermode.c for the higher-level API this backs. */

/* Selectors, matching gdt.c's struct gdt_table layout (see gdt.h) --
 * RPL=3 baked in, since these are only ever loaded while entering ring 3. */
#define SEL_UCODE 0x1B /* 0x18 | 3 */
#define SEL_UDATA 0x23 /* 0x20 | 3 */

.text

/* void arch_enter_usermode(uint64_t entry, uint64_t user_rsp, uint64_t *save_area);
 * (SysV: entry in rdi, user_rsp in rsi, save_area in rdx.)
 *
 * Saves the current (kernel) callee-saved registers and rsp into
 * `save_area` (7 quads: rbx, rbp, r12, r13, r14, r15, rsp -- a
 * hand-rolled setjmp, one slot per task, see usermode.h's own comment on
 * struct usertask.kernel_resume for why this can't be a single shared
 * slot) -- then builds an iretq frame and jumps into ring 3. Never
 * returns directly to its caller; the only way back is
 * arch_return_to_kernel's `ret`, landing exactly at this call's return
 * address as if arch_enter_usermode had returned normally. */
.global arch_enter_usermode
arch_enter_usermode:
    mov %rbx, 0(%rdx)
    mov %rbp, 8(%rdx)
    mov %r12, 16(%rdx)
    mov %r13, 24(%rdx)
    mov %r14, 32(%rdx)
    mov %r15, 40(%rdx)
    mov %rsp, 48(%rdx) /* points at our own return address right now */

    /* iretq pops, in order: rip, cs, rflags, rsp, ss -- so push in the
     * reverse order. */
    pushq $SEL_UDATA
    pushq %rsi           /* user_rsp */
    pushq $0x202         /* rflags: reserved bit 1 set, IF=1 */
    pushq $SEL_UCODE
    pushq %rdi           /* entry */
    iretq

/* void arch_return_to_kernel(int status, uint64_t *save_area);
 * (SysV: status in edi, save_area in rsi.)
 *
 * Restores the context arch_enter_usermode/arch_resume_process saved
 * into `save_area` and resumes it via `ret` -- from the perspective of
 * whoever called them, it just returned. Never returns to its own
 * caller. */
.global arch_return_to_kernel
arch_return_to_kernel:
    mov %edi, kernel_resume_status(%rip)
    mov 0(%rsi), %rbx
    mov 8(%rsi), %rbp
    mov 16(%rsi), %r12
    mov 24(%rsi), %r13
    mov 32(%rsi), %r14
    mov 40(%rsi), %r15
    mov 48(%rsi), %rsp
    ret

/* void arch_resume_process(uint64_t saved_rsp, uint64_t *save_area);
 * (SysV: saved_rsp in rdi, save_area in rsi.)
 *
 * Counterpart to arch_enter_usermode for a process that has already run
 * at least once (M13): rather than building a fresh iretq frame at an
 * entry point, resumes exactly where it left off. `saved_rsp` is the
 * value %rsp held at the moment this process's execution was last
 * captured (by fork() building an initial frame for its child, or --
 * once preemption exists -- by a timer interrupt) -- i.e. the address of
 * its own already-populated struct interrupt_frame, sitting on its own
 * kernel stack exactly as isr_common_stub/syscall_common_stub left it.
 * No register state needs copying anywhere; setting %rsp there and
 * falling into the same pop-registers-then-iretq tail those stubs use is
 * enough. Saves the current (kernel) context into `save_area` first,
 * exactly like arch_enter_usermode -- the only way back is the same
 * arch_return_to_kernel() `ret`. */
.extern common_return_from_interrupt
.global arch_resume_process
arch_resume_process:
    mov %rbx, 0(%rsi)
    mov %rbp, 8(%rsi)
    mov %r12, 16(%rsi)
    mov %r13, 24(%rsi)
    mov %r14, 32(%rsi)
    mov %r15, 40(%rsi)
    mov %rsp, 48(%rsi)

    mov %rdi, %rsp
    jmp common_return_from_interrupt

.section .bss
.align 8
.global kernel_resume_status
kernel_resume_status:
    .skip 4

.section .note.GNU-stack,"",@progbits
