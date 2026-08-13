#ifndef ARCH_X86_64_GDT_H
#define ARCH_X86_64_GDT_H

#include <stdint.h>

/* Flat GDT: ring 0 code/data, ring 3 code/data (M10, for userspace), and a
 * TSS purely to satisfy the CPU's requirement that one be loaded in long
 * mode -- until M10, also unused beyond that requirement. Selectors are
 * fixed by the layout of struct gdt_table in gdt.c. The ring-3 selectors
 * have RPL=3 baked in, since they're only ever loaded while actually
 * running at CPL 3. */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE (0x18 | 3)
#define SEL_UDATA (0x20 | 3)
#define SEL_TSS 0x28

void gdt_init(void);

/* Sets the kernel stack pointer (TSS.RSP0) the CPU switches to
 * automatically on any interrupt/exception/syscall that changes
 * privilege level from ring 3 to ring 0 -- must be valid before ever
 * entering ring 3 (see arch/x86_64/usermode.c), and updated per task
 * once more than one exists. */
void tss_set_kernel_stack(uint64_t rsp0);

#endif
