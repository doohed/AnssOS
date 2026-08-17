/* Embeds the built hello/crash/malloctest/filetest/dirtest/forktest/
 * forkchild/preempttest/termtest/readdirtest userland test payloads (see
 * scripts/build-userland.sh, which drops their built ELF bytes at
 * <name>.elf.bin right next to this file before the kernel itself is
 * built) directly into the kernel image, so main.c's self-test wiring
 * can write them onto the VFS without any separate host-side
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

.global malloctest_elf_start
.global malloctest_elf_end
malloctest_elf_start:
.incbin "src/exec/malloctest.elf.bin"
malloctest_elf_end:

.global filetest_elf_start
.global filetest_elf_end
filetest_elf_start:
.incbin "src/exec/filetest.elf.bin"
filetest_elf_end:

.global dirtest_elf_start
.global dirtest_elf_end
dirtest_elf_start:
.incbin "src/exec/dirtest.elf.bin"
dirtest_elf_end:

.global forktest_elf_start
.global forktest_elf_end
forktest_elf_start:
.incbin "src/exec/forktest.elf.bin"
forktest_elf_end:

.global forkchild_elf_start
.global forkchild_elf_end
forkchild_elf_start:
.incbin "src/exec/forkchild.elf.bin"
forkchild_elf_end:

.global preempttest_elf_start
.global preempttest_elf_end
preempttest_elf_start:
.incbin "src/exec/preempttest.elf.bin"
preempttest_elf_end:

.global termtest_elf_start
.global termtest_elf_end
termtest_elf_start:
.incbin "src/exec/termtest.elf.bin"
termtest_elf_end:

.global readdirtest_elf_start
.global readdirtest_elf_end
readdirtest_elf_start:
.incbin "src/exec/readdirtest.elf.bin"
readdirtest_elf_end:

.global scarf_elf_start
.global scarf_elf_end
scarf_elf_start:
.incbin "src/exec/scarf.elf.bin"
scarf_elf_end:

.global play_elf_start
.global play_elf_end
play_elf_start:
.incbin "src/exec/play.elf.bin"
play_elf_end:

/* Not an ELF -- raw WAV bytes (see scripts/gen-test-tone.py), a fixture
 * for `play` the same way filetest.txt is a fixture for filetest.bin. */
.global testtone_wav_start
.global testtone_wav_end
testtone_wav_start:
.incbin "src/exec/testtone.wav.bin"
testtone_wav_end:

.global pipetest_elf_start
.global pipetest_elf_end
pipetest_elf_start:
.incbin "src/exec/pipetest.elf.bin"
pipetest_elf_end:

.global sh_elf_start
.global sh_elf_end
sh_elf_start:
.incbin "src/exec/sh.elf.bin"
sh_elf_end:

.global tile_elf_start
.global tile_elf_end
tile_elf_start:
.incbin "src/exec/tile.elf.bin"
tile_elf_end:

.section .note.GNU-stack,"",@progbits
