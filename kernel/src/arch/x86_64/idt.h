#ifndef ARCH_X86_64_IDT_H
#define ARCH_X86_64_IDT_H

#include <stdint.h>

/* Register state as pushed by isr_common_stub (see isr.S) and
 * syscall_common_stub (see syscall.S), in the order the C handler sees
 * it. The stubs `push` each register in the order r15, r14, ..., rbx,
 * rax -- since `push` stores at the *new* (decremented) %rsp, the *last*
 * register pushed (rax) ends up at the *lowest* address, i.e. offset 0
 * from the frame pointer the stubs pass in %rdi. This must therefore be
 * the reverse of the push order for the field offsets to actually match
 * the pushed registers; only the GPR block needs reversing -- vector/
 * error_code/rip/cs/rflags/rsp/ss are pushed after (GPRs) or before
 * (rip..ss, by hardware) in an order that already matches declaration
 * order here. */
struct interrupt_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, r8;
    uint64_t r9, r10, r11, r12, r13, r14, r15;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

void idt_init(void);

/* Registers a handler for hardware IRQ `irq` (0-15, pre-remap numbering --
 * see arch/x86_64/pic.h for how that maps to interrupt vectors). EOI is
 * sent automatically after the handler returns; handlers must not send
 * it themselves. Unregistered IRQs are silently EOI'd and ignored (a
 * safe default for spurious interrupts). */
void irq_register(uint8_t irq, void (*handler)(void));

#endif
