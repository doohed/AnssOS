#ifndef ARCH_X86_64_GDT_H
#define ARCH_X86_64_GDT_H

/* Kernel-only flat GDT (ring 0 code/data, plus placeholder ring 3 */
/* descriptors for when we eventually have userspace) and a TSS purely to */
/* satisfy the CPU's requirement that one be loaded in long mode. */

void gdt_init(void);

#endif
