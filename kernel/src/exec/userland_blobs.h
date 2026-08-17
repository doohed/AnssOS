#ifndef EXEC_USERLAND_BLOBS_H
#define EXEC_USERLAND_BLOBS_H

/* The built hello/crash/malloctest/filetest/dirtest/forktest/forkchild/
 * preempttest/termtest/readdirtest test payloads, embedded via
 * userland_blobs.S -- see scripts/build-userland.sh. */
extern const unsigned char hello_elf_start[];
extern const unsigned char hello_elf_end[];
extern const unsigned char crash_elf_start[];
extern const unsigned char crash_elf_end[];
extern const unsigned char malloctest_elf_start[];
extern const unsigned char malloctest_elf_end[];
extern const unsigned char filetest_elf_start[];
extern const unsigned char filetest_elf_end[];
extern const unsigned char dirtest_elf_start[];
extern const unsigned char dirtest_elf_end[];
extern const unsigned char forktest_elf_start[];
extern const unsigned char forktest_elf_end[];
extern const unsigned char forkchild_elf_start[];
extern const unsigned char forkchild_elf_end[];
extern const unsigned char preempttest_elf_start[];
extern const unsigned char preempttest_elf_end[];
extern const unsigned char termtest_elf_start[];
extern const unsigned char termtest_elf_end[];
extern const unsigned char readdirtest_elf_start[];
extern const unsigned char readdirtest_elf_end[];
extern const unsigned char scarf_elf_start[];
extern const unsigned char scarf_elf_end[];
extern const unsigned char play_elf_start[];
extern const unsigned char play_elf_end[];
extern const unsigned char testtone_wav_start[];
extern const unsigned char testtone_wav_end[];
extern const unsigned char pipetest_elf_start[];
extern const unsigned char pipetest_elf_end[];
extern const unsigned char sh_elf_start[];
extern const unsigned char sh_elf_end[];
extern const unsigned char tile_elf_start[];
extern const unsigned char tile_elf_end[];

#endif
