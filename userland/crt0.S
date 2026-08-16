/* Minimal C runtime start for the hand-rolled userland payloads -- see
 * kernel/src/exec/elf.c. Reads argc/argv off the System V process
 * initialization stack elf_load() builds (RSP points at argc, with the
 * argv pointer array immediately above it), passes them to main(), then
 * exit()s with its return value.
 *
 * The stack has to be 16-byte aligned at a call boundary, and RSP points
 * at argc rather than a return address on entry -- so it's read first,
 * then aligned, rather than the other way round. */

.global _start
_start:
    xor %ebp, %ebp     /* mark the outermost frame */
    mov (%rsp), %rdi   /* argc */
    lea 8(%rsp), %rsi  /* argv */
    and $-16, %rsp
    call main
    mov %eax, %edi
    call exit
1:  jmp 1b /* unreachable -- exit() never returns */

.section .note.GNU-stack,"",@progbits
