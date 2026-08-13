#ifndef EXEC_SYSCALL_H
#define EXEC_SYSCALL_H

#include "../arch/x86_64/idt.h"

/* Called from the vector-0x80 trampoline (syscall.S) with `frame`
 * pointing at the interrupted task's saved registers. Reads the syscall
 * number from frame->rax and args from frame->rdi/rsi/rdx (a System-V-
 * ish convention), writes the return value back into frame->rax. */
void syscall_dispatch(struct interrupt_frame *frame);

#endif
