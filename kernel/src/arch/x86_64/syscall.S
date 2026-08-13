/* The syscall entry trampoline (vector 0x80, DPL=3 -- registered in
 * idt.c). Structurally identical to isr_common_stub in isr.S (same
 * struct interrupt_frame push order), just calling syscall_dispatch()
 * instead of isr_handler(). syscall_dispatch() writes its return value
 * into frame->rax -- since that's the same stack slot `pop %rax` below
 * reads from, the modified value correctly ends up back in %rax before
 * iretq. */

.text

.global isr_stub_syscall
isr_stub_syscall:
    push $0   /* dummy error code -- software interrupts don't push one */
    push $0x80
    jmp syscall_common_stub

.extern syscall_dispatch

syscall_common_stub:
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax

    mov %rsp, %rdi          /* struct interrupt_frame *frame */
    call syscall_dispatch

    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    add $16, %rsp            /* discard vector + error_code */
    iretq

.section .note.GNU-stack,"",@progbits
