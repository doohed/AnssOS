#ifndef EXEC_USERLAND_BLOBS_H
#define EXEC_USERLAND_BLOBS_H

/* The built hello/crash/malloctest/filetest/dirtest/forktest/forkchild/
 * preempttest test payloads, embedded via userland_blobs.S -- see
 * scripts/build-userland.sh. */
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

#endif
