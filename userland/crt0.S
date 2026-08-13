/* Minimal C runtime start for the hand-rolled userland test payloads
 * (hello/crash -- see kernel/src/exec/elf.c and the M10 plan). Calls
 * main(), then exit()s with its return value. */

.global _start
_start:
    xor %ebp, %ebp /* mark the outermost frame */
    call main
    mov %eax, %edi
    call exit
1:  jmp 1b /* unreachable -- exit() never returns */

.section .note.GNU-stack,"",@progbits
