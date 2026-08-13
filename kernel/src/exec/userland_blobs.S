/* Embeds the built hello/crash userland test payloads (see
 * scripts/build-userland.sh, which drops their built ELF bytes at
 * hello.elf.bin/crash.elf.bin right next to this file before the kernel
 * itself is built) directly into the kernel image, so the M10 self-test
 * in main.c can write them onto the VFS without any separate host-side
 * provisioning step. */

.section .rodata

.global hello_elf_start
.global hello_elf_end
hello_elf_start:
.incbin "src/exec/hello.elf.bin"
hello_elf_end:

.global crash_elf_start
.global crash_elf_end
crash_elf_start:
.incbin "src/exec/crash.elf.bin"
crash_elf_end:

.section .note.GNU-stack,"",@progbits
