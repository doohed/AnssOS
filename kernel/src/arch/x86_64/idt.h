#ifndef ARCH_X86_64_IDT_H
#define ARCH_X86_64_IDT_H

#include <stdint.h>

/* Register state as pushed by isr_common_stub (see isr.S), in the order */
/* the C handler sees it (isr_handler pops nothing, so this must match */
/* the push order there exactly, reversed). */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

void idt_init(void);

#endif
