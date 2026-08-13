#ifndef EXEC_ELF_H
#define EXEC_ELF_H

#include "../arch/x86_64/usermode.h"

#include <stddef.h>
#include <stdint.h>

/* Parses a static, non-PIE ELF64 x86_64 executable already fully in
 * memory (e.g. a VFS file's vnode->data/size -- see fs/vfs.h) and builds
 * a ready-to-run struct usertask: a fresh address space with every
 * PT_LOAD segment mapped in (user-accessible, read-write -- no W^X/NX
 * enforcement in this first pass), plus a user stack and a kernel stack.
 * Returns 0 on success (with `out` populated) or -1 on a malformed/
 * unsupported file (reports why via kprintf). */
int elf_load(const uint8_t *image, size_t image_size, struct usertask *out);

#endif
