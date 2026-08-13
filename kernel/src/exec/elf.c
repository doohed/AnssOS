#include "elf.h"
#include "../arch/x86_64/usermode.h"
#include "../drivers/serial.h"
#include "../drivers/tty.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

#include <stddef.h>
#include <stdint.h>

#define PT_LOAD 1
#define EM_X86_64 62
#define ELFCLASS64 2
#define ELFDATA2LSB 1

struct __attribute__((packed)) elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct __attribute__((packed)) elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* Fixed user-space layout: plenty of headroom below the canonical
 * boundary (0x0000800000000000) so it can never collide with a
 * reasonably-linked non-PIE binary's own segments (those typically start
 * around 0x400000). */
#define USER_STACK_TOP 0x0000700000000000ull
#define USER_STACK_PAGES 4 /* 16 KiB */

int elf_load(const uint8_t *image, size_t image_size, struct usertask *out) {
    memset(out, 0, sizeof(*out)); /* open_files[].vnode == NULL (free) for every slot. */

    if (image_size < sizeof(struct elf64_ehdr)) {
        kprintf("elf: file too small to be an ELF header\n");
        return -1;
    }

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
        eh->e_ident[3] != 'F') {
        kprintf("elf: not an ELF file\n");
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) {
        kprintf("elf: not a 64-bit little-endian ELF\n");
        return -1;
    }
    if (eh->e_machine != EM_X86_64) {
        kprintf("elf: not an x86_64 binary\n");
        return -1;
    }
    if ((uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > image_size) {
        kprintf("elf: program header table out of bounds\n");
        return -1;
    }

    struct addr_space as = vmm_new_address_space();
    const struct elf64_phdr *phdrs = (const struct elf64_phdr *)(image + eh->e_phoff);
    uint64_t heap_base = 0; /* Highest PT_LOAD segment's mapped end -- see below. */

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > image_size || ph->p_filesz > ph->p_memsz) {
            kprintf("elf: PT_LOAD segment %u out of bounds\n", i);
            return -1;
        }

        uint64_t seg_start = ph->p_vaddr & ~(uint64_t)(PMM_PAGE_SIZE - 1);
        uint64_t seg_offset = ph->p_vaddr - seg_start;
        uint64_t seg_end = ph->p_vaddr + ph->p_memsz;
        uint64_t page_count = (seg_end - seg_start + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

        uint64_t base_phys = pmm_alloc_pages(page_count);
        if (base_phys == 0) {
            kprintf("elf: out of memory for segment %u (%lu pages)\n", i, page_count);
            return -1;
        }

        uint8_t *seg_kernel_virt = vmm_phys_to_virt(base_phys);
        memset(seg_kernel_virt, 0, page_count * PMM_PAGE_SIZE);
        memcpy(seg_kernel_virt + seg_offset, image + ph->p_offset, ph->p_filesz);

        for (uint64_t p = 0; p < page_count; p++) {
            vmm_map(&as, seg_start + p * PMM_PAGE_SIZE, base_phys + p * PMM_PAGE_SIZE,
                    PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        }

        uint64_t seg_mapped_end = seg_start + page_count * PMM_PAGE_SIZE;
        if (seg_mapped_end > heap_base) {
            heap_base = seg_mapped_end;
        }
    }

    uint64_t user_stack_phys = pmm_alloc_pages(USER_STACK_PAGES);
    if (user_stack_phys == 0) {
        kprintf("elf: out of memory for the user stack\n");
        return -1;
    }
    memset(vmm_phys_to_virt(user_stack_phys), 0, USER_STACK_PAGES * PMM_PAGE_SIZE);
    uint64_t user_stack_base = USER_STACK_TOP - USER_STACK_PAGES * PMM_PAGE_SIZE;
    for (uint64_t p = 0; p < USER_STACK_PAGES; p++) {
        vmm_map(&as, user_stack_base + p * PMM_PAGE_SIZE, user_stack_phys + p * PMM_PAGE_SIZE,
                PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    /* The kernel stack (TSS.RSP0) is only ever touched by kernel code
     * running this task's syscalls/faults -- an ordinary kmalloc buffer
     * is fine, no user mapping needed, and it's already reachable from
     * every address space since the HHDM is part of the shared higher
     * half every vmm_new_address_space() copies in. */
    uint8_t *kernel_stack = kmalloc(ELF_KERNEL_STACK_SIZE);
    if (kernel_stack == NULL) {
        kprintf("elf: out of memory for the kernel stack\n");
        return -1;
    }

    if (heap_base == 0) {
        heap_base = 0x600000; /* No PT_LOAD segments (degenerate) -- a safe fallback. */
    }

    out->entry = eh->e_entry;
    out->user_stack_top = USER_STACK_TOP;
    out->kernel_stack_top = (uint64_t)(uintptr_t)(kernel_stack + ELF_KERNEL_STACK_SIZE);
    out->as = as;
    out->heap_start = heap_base;
    out->heap_end = heap_base; /* Zero-size until the first brk(). */
    out->cwd = vfs_root();
    out->termios.c_lflag = ICANON | ECHO; /* Today's actual default behavior, made explicit. */
    out->termios.c_cc[VMIN] = 1;
    out->termios.c_cc[VTIME] = 0;
    return 0;
}
