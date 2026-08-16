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

/* Caps on the argument vector. Generous next to a 16 KiB stack (16 args
 * of 128 bytes is 2 KiB), and bounded so a caller can't push the initial
 * stack down past the pages actually mapped for it. */
#define MAX_ARGS 16
#define MAX_ARG_LEN 128

int elf_load(const uint8_t *image, size_t image_size, int argc, const char *const *argv,
             struct vnode *cwd, struct usertask *out) {
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

    /* Build the System V AMD64 process-initialization stack. At entry RSP
     * points at argc, immediately followed by the argv pointer array, its
     * NULL terminator, an empty envp array and an empty auxv vector; the
     * argument strings themselves sit higher up. crt0.S reads argc/argv
     * straight off this, which is why adding arguments changes the entry
     * ABI for every payload at once.
     *
     * Written through the HHDM rather than through the new address space:
     * the stack pages come from one pmm_alloc_pages() run, so they're
     * physically contiguous and `stack_kernel + (uaddr - user_stack_base)`
     * addresses any of them without switching CR3. */
    uint8_t *stack_kernel = vmm_phys_to_virt(user_stack_phys);
    uint64_t user_sp = USER_STACK_TOP;
    uint64_t arg_ptrs[MAX_ARGS];

    int nargs = argc;
    if (nargs < 0) {
        nargs = 0;
    }
    if (nargs > MAX_ARGS) {
        nargs = MAX_ARGS;
    }

    for (int i = nargs - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]);
        if (len > MAX_ARG_LEN - 1) {
            len = MAX_ARG_LEN - 1;
        }
        user_sp -= len + 1;
        char *dest = (char *)(stack_kernel + (user_sp - user_stack_base));
        memcpy(dest, argv[i], len);
        dest[len] = '\0';
        arg_ptrs[i] = user_sp;
    }

    /* argc, argv[nargs], the argv NULL, the envp NULL, and AT_NULL's
     * key/value pair. Aligned down afterwards, which only ever leaves
     * more room, never less. */
    user_sp -= (1 + (uint64_t)nargs + 1 + 1 + 2) * 8;
    user_sp &= ~(uint64_t)0xf;

    if (user_sp < user_stack_base) {
        kprintf("elf: argument vector does not fit in the user stack\n");
        return -1;
    }

    uint64_t *frame = (uint64_t *)(stack_kernel + (user_sp - user_stack_base));
    size_t slot = 0;
    frame[slot++] = (uint64_t)nargs;
    for (int i = 0; i < nargs; i++) {
        frame[slot++] = arg_ptrs[i];
    }
    frame[slot++] = 0; /* argv terminator */
    frame[slot++] = 0; /* envp[0] -- no environment yet */
    frame[slot++] = 0; /* auxv AT_NULL key */
    frame[slot++] = 0; /* auxv AT_NULL value */

    out->entry = eh->e_entry;
    out->user_stack_top = user_sp;
    out->kernel_stack_top = (uint64_t)(uintptr_t)(kernel_stack + ELF_KERNEL_STACK_SIZE);
    out->as = as;
    out->heap_start = heap_base;
    out->heap_end = heap_base; /* Zero-size until the first brk(). */
    /* Inherits the launching shell's directory (or the exec'ing task's).
     * M12 deliberately started every task at the root instead; that made
     * `scarf .` meaningless, since "." always resolved to / no matter
     * where it was launched from. */
    out->cwd = cwd != NULL ? cwd : vfs_root();
    out->termios.c_lflag = ICANON | ECHO; /* Today's actual default behavior, made explicit. */
    out->termios.c_cc[VMIN] = 1;
    out->termios.c_cc[VTIME] = 0;
    return 0;
}
