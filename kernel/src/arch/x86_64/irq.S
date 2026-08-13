/* One trampoline per hardware IRQ, remapped by arch/x86_64/pic.c to */
/* vectors 32-47 (IRQ0-15). Hardware IRQs never push an error code, so */
/* every stub here has the same shape as isr.S's isr_no_err -- a dummy 0 */
/* pushed first so the C-side frame layout (struct interrupt_frame) stays */
/* uniform regardless of which vector fired. */

.text

.macro irq_stub vec
.global irq_stub_\vec
irq_stub_\vec:
    push $0
    push $\vec
    jmp irq_common_stub
.endm

irq_stub 32
irq_stub 33
irq_stub 34
irq_stub 35
irq_stub 36
irq_stub 37
irq_stub 38
irq_stub 39
irq_stub 40
irq_stub 41
irq_stub 42
irq_stub 43
irq_stub 44
irq_stub 45
irq_stub 46
irq_stub 47

.extern irq_handler

irq_common_stub:
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
    call irq_handler

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

.section .rodata
.global irq_stub_table
irq_stub_table:
    .quad irq_stub_32
    .quad irq_stub_33
    .quad irq_stub_34
    .quad irq_stub_35
    .quad irq_stub_36
    .quad irq_stub_37
    .quad irq_stub_38
    .quad irq_stub_39
    .quad irq_stub_40
    .quad irq_stub_41
    .quad irq_stub_42
    .quad irq_stub_43
    .quad irq_stub_44
    .quad irq_stub_45
    .quad irq_stub_46
    .quad irq_stub_47

.section .note.GNU-stack,"",@progbits
