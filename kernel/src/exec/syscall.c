#include "syscall.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/usermode.h"
#include "../console/fbconsole.h"
#include "../drivers/serial.h"
#include "../drivers/tty.h"
#include "../drivers/virtio/virtio_input.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "process.h"

#include <stddef.h>
#include <stdint.h>

/* Linux's syscall numbers for this initial handful -- reusing them costs
 * nothing now and avoids a renumbering exercise later if AnssOS ever
 * wants to run real static Linux binaries (a distant aspiration, but a
 * free one to keep open by not inventing our own numbering here). */
#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_ioctl 16
#define SYS_lseek 8
#define SYS_brk 12
#define SYS_getpid 39
#define SYS_chdir 80
#define SYS_mkdir 83
#define SYS_fork 57
#define SYS_execve 59
#define SYS_wait4 61
#define SYS_exit 60
#define SYS_getdents 217

/* ioctl() requests this project actually understands -- Linux's real
 * TCGETS/TCSETS values, so a program built the standard way (get
 * current termios, tweak flags, set them back) works unmodified. */
#define TCGETS 0x5401
#define TCSETS 0x5402

/* A task's brk-managed heap never grows past this many bytes past
 * heap_start -- a simple OOM guard, not a real ulimit. */
#define HEAP_MAX_BYTES (4 * 1024 * 1024)

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x40
#define O_TRUNC 0x200

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* A deliberately simplified getdents() -- see sys_getdents_impl() --
 * one entry per call, not real Linux getdents64's batched-buffer ABI.
 * d_type values match Linux's real ones anyway, for free compat. */
#define DT_DIR 4
#define DT_REG 8

struct dirent {
    uint8_t d_type;
    char d_name[VFS_NAME_MAX];
};

/* Coarse user-pointer validation: real hardware/OS-grade safety needs a
 * fault-recovering copy_from_user(); a bad-but-in-range user pointer
 * still just crashes that one task (see idt.c's non-fatal user-fault
 * path) rather than the kernel, so this first pass only needs to reject
 * pointers that would reach into kernel space. */
static int user_ptr_ok(const void *ptr, size_t len) {
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    return addr < 0x0000800000000000ull && addr + len < 0x0000800000000000ull;
}

/* Real (non-stdio) file descriptors are `3 + index` into the current
 * task's open_files[] -- returns the slot, or NULL if `fd` isn't a
 * currently-open one. */
static struct open_file *open_file_for(int fd) {
    struct usertask *task = usermode_current_task();
    int idx = fd - 3;
    if (task == NULL || idx < 0 || idx >= MAX_OPEN_FILES || task->open_files[idx].vnode == NULL) {
        return NULL;
    }
    return &task->open_files[idx];
}

static int64_t sys_open_impl(const char *path, int flags) {
    struct usertask *task = usermode_current_task();
    int access_mode = flags & ~(O_CREAT | O_TRUNC);
    if (task == NULL || !user_ptr_ok(path, 1) ||
        (access_mode != O_RDONLY && access_mode != O_WRONLY)) {
        return -1;
    }

    struct vnode *node = vfs_resolve(task->cwd, path);
    if (node == NULL && (flags & O_CREAT)) {
        if (vfs_create_file(task->cwd, path) != 0) {
            return -1;
        }
        node = vfs_resolve(task->cwd, path);
    }
    if (node == NULL) {
        return -1;
    }
    /* A directory can only be opened O_RDONLY (no O_CREAT/O_WRONLY --
     * creating inside one already goes through mkdir()/open(..., O_CREAT),
     * not this path) -- see sys_getdents_impl() for what an open
     * directory fd is actually good for. */
    if (node->type == VNODE_DIR && flags != O_RDONLY) {
        return -1;
    }

    /* O_TRUNC: drop the existing content before the first write. Without
     * this, sys_write_impl() only ever grows a file (it writes at an
     * offset and extends), so saving a file that got *shorter* would
     * leave the tail of the previous version behind -- which any editor
     * (userland/scarf.c) does constantly. Only the length is reset; the
     * allocation is left alone for the coming writes to reuse. */
    if ((flags & O_TRUNC) && node->type == VNODE_FILE && access_mode == O_WRONLY) {
        node->size = 0;
    }

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (task->open_files[i].vnode == NULL) {
            task->open_files[i].vnode = node;
            task->open_files[i].offset = 0;
            task->open_files[i].writable = (access_mode == O_WRONLY);
            return 3 + i;
        }
    }
    return -1; /* too many open files */
}

static int64_t sys_close_impl(int fd) {
    struct open_file *f = open_file_for(fd);
    if (f == NULL) {
        return -1;
    }
    f->vnode = NULL;
    return 0;
}

static int64_t sys_chdir_impl(const char *path) {
    struct usertask *task = usermode_current_task();
    if (task == NULL || !user_ptr_ok(path, 1)) {
        return -1;
    }
    struct vnode *node = vfs_resolve(task->cwd, path);
    if (node == NULL || node->type != VNODE_DIR) {
        return -1;
    }
    task->cwd = node;
    return 0;
}

static int64_t sys_mkdir_impl(const char *path) {
    struct usertask *task = usermode_current_task();
    if (task == NULL || !user_ptr_ok(path, 1)) {
        return -1;
    }
    return vfs_mkdir(task->cwd, path) == 0 ? 0 : -1;
}

static int64_t sys_lseek_impl(int fd, int64_t offset, int whence) {
    struct open_file *f = open_file_for(fd);
    if (f == NULL) {
        return -1;
    }

    /* SEEK_END has no natural meaning for a directory fd (offset there
     * counts children, not bytes -- see sys_getdents_impl()) without a
     * full count, kept minimal by just rejecting it; SEEK_SET/SEEK_CUR
     * work the same for both (SEEK_SET(0) is rewinddir()'s primitive). */
    if (f->vnode->type == VNODE_DIR && whence == SEEK_END) {
        return -1;
    }

    int64_t base;
    switch (whence) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = (int64_t)f->offset;
            break;
        case SEEK_END:
            base = (int64_t)f->vnode->size;
            break;
        default:
            return -1;
    }

    int64_t new_offset = base + offset;
    if (new_offset < 0) {
        return -1;
    }
    f->offset = (size_t)new_offset;
    return new_offset;
}

/* Walks `f->vnode->children` (following ->sibling, matching fs/vfs.c's
 * own vfs_list() traversal) `f->offset` entries in, fills `*out`, and
 * advances the offset -- one entry per call, not real Linux getdents64's
 * batched-buffer ABI (see the dirent/DT_* comment above). Returns 1 (an
 * entry was written), 0 (past the last child -- end of directory), or
 * -1 (bad fd / not a directory). */
static int64_t sys_getdents_impl(int fd, struct dirent *out) {
    struct open_file *f = open_file_for(fd);
    if (f == NULL || f->vnode->type != VNODE_DIR || !user_ptr_ok(out, sizeof(struct dirent))) {
        return -1;
    }

    struct vnode *child = f->vnode->children;
    for (size_t i = 0; i < f->offset && child != NULL; i++) {
        child = child->sibling;
    }
    if (child == NULL) {
        return 0;
    }

    out->d_type = (child->type == VNODE_DIR) ? DT_DIR : DT_REG;
    memcpy(out->d_name, child->name, VFS_NAME_MAX);
    f->offset++;
    return 1;
}

static int64_t sys_write_impl(int fd, const void *buf, size_t len) {
    if (!user_ptr_ok(buf, len)) {
        return -1;
    }
    if (fd == 1 || fd == 2) {
        const char *bytes = buf;
        /* One write() from userland becomes exactly one virtio-gpu flush
         * rather than one per character. A full-screen redraw
         * (userland/scarf.c) is thousands of bytes, and a virtqueue round
         * trip each was slow enough to make the editor unusable -- see
         * fbconsole_begin_batch(). Purely a batching change: the bytes,
         * and the serial output, are identical either way. */
        fbconsole_begin_batch();
        for (size_t i = 0; i < len; i++) {
            kprintf("%c", bytes[i]);
        }
        fbconsole_end_batch();
        return (int64_t)len;
    }

    struct open_file *f = open_file_for(fd);
    if (f == NULL || !f->writable) {
        return -1;
    }
    struct vnode *node = f->vnode;

    /* Growing the file: kmalloc a bigger buffer, copy the existing
     * content plus a zero-filled gap if `offset` starts past the
     * current end, free the old buffer -- the same operation
     * fs/vfs.c's own vfs_write_bytes() does for a whole-file overwrite,
     * just done here directly (matching fs/blkfs.c's existing precedent
     * of touching vnode->data/size/capacity directly) since this needs
     * to write at an arbitrary offset, not just from the start. */
    size_t new_size = f->offset + len;
    if (new_size > node->size) {
        uint8_t *grown = kmalloc(new_size);
        if (grown == NULL) {
            return -1;
        }
        if (node->data != NULL && node->size > 0) {
            memcpy(grown, node->data, node->size);
        }
        if (f->offset > node->size) {
            memset(grown + node->size, 0, f->offset - node->size);
        }
        kfree(node->data);
        node->data = grown;
        node->size = new_size;
        node->capacity = new_size;
    }
    memcpy(node->data + f->offset, buf, len);
    f->offset += len;
    return (int64_t)len;
}

static int poll_console_char(void) {
    int c = virtio_input_poll_char();
    if (c < 0) {
        c = serial_poll_char();
    }
    return c;
}

/* fd 0 in canonical mode (the default -- see ICANON in drivers/tty.h) is
 * line-buffered, matching the shell's own read_line(): polls both input
 * sources, echoes as it goes, stops at Enter or `len` bytes. In raw mode
 * (ICANON cleared via TCSETS -- see sys_ioctl_impl()) there's no echo
 * and no backspace handling: blocks until at least one byte is
 * available (VMIN=1), then drains whatever else is immediately ready up
 * to `len` without blocking further (VTIME=0) -- the one VMIN/VTIME
 * combination actually implemented, since it's the one raw-mode
 * programs use. fd >= 3 reads straight from the open vnode's buffer at
 * the tracked offset (files only -- see sys_open_impl()/open_file_for()
 * for the directory case). */
static int64_t sys_read_impl(int fd, void *buf, size_t len) {
    if (!user_ptr_ok(buf, len)) {
        return -1;
    }

    if (fd != 0) {
        struct open_file *f = open_file_for(fd);
        if (f == NULL || f->vnode->type != VNODE_FILE) {
            return -1;
        }
        struct vnode *node = f->vnode;
        size_t remaining = f->offset < node->size ? node->size - f->offset : 0;
        size_t n = len < remaining ? len : remaining;
        if (n > 0) {
            memcpy(buf, node->data + f->offset, n);
            f->offset += n;
        }
        return (int64_t)n;
    }

    struct usertask *task = usermode_current_task();
    int raw = task != NULL && !(task->termios.c_lflag & ICANON);
    char *bytes = buf;
    size_t n = 0;

    if (raw) {
        while (n < len) {
            int c = poll_console_char();
            if (c < 0) {
                if (n > 0) {
                    break; /* VMIN=1/VTIME=0: at least one byte, don't wait for more. */
                }
                asm volatile("pause");
                continue;
            }
            bytes[n++] = (char)c;
        }
        return (int64_t)n;
    }

    while (n < len) {
        int c = poll_console_char();
        if (c < 0) {
            asm volatile("pause");
            continue;
        }
        if (c == '\n' || c == '\r') {
            kprintf("\n");
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (n > 0) {
                n--;
                kprintf("\b \b");
            }
            continue;
        }
        bytes[n++] = (char)c;
        kprintf("%c", c);
    }
    return (int64_t)n;
}

/* `new_end == 0` queries the current break without changing it (the
 * glibc/musl-style `brk()` convention). Never shrinks -- once a page is
 * mapped for the heap it stays mapped, same "no giving memory back"
 * pragmatism as mm/heap.c's kmalloc/kfree. Grows one page at a time up
 * to HEAP_MAX_BYTES past heap_start, as a simple OOM guard. */
static int64_t sys_brk_impl(uint64_t new_end) {
    struct usertask *task = usermode_current_task();
    if (task == NULL) {
        return -1;
    }
    if (new_end == 0 || new_end <= task->heap_end) {
        return (int64_t)task->heap_end;
    }
    if (new_end - task->heap_start > HEAP_MAX_BYTES) {
        return -1;
    }

    while (task->heap_end < new_end) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            return -1;
        }
        memset(vmm_phys_to_virt(phys), 0, PMM_PAGE_SIZE);
        vmm_map(&task->as, task->heap_end, phys, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        task->heap_end += PMM_PAGE_SIZE;
    }
    return (int64_t)task->heap_end;
}

/* Only the console (fd 0/1/2 -- they all share the same task->termios)
 * and TCGETS/TCSETS are understood; anything else is rejected, matching
 * a real ENOTTY-style failure. */
static int64_t sys_ioctl_impl(int fd, uint64_t request, void *argp) {
    struct usertask *task = usermode_current_task();
    if (task == NULL || (fd != 0 && fd != 1 && fd != 2)) {
        return -1;
    }

    /* The buffer size to validate depends on the request -- a struct
     * winsize is 8 bytes where a struct termios is 36, so checking one
     * fixed size up front (as this did when TCGETS/TCSETS were the only
     * requests) would reject perfectly valid winsize pointers sitting
     * near the end of a mapped page. */
    if (request == TCGETS) {
        if (!user_ptr_ok(argp, sizeof(struct k_termios))) {
            return -1;
        }
        *(struct k_termios *)argp = task->termios;
        return 0;
    }
    if (request == TCSETS) {
        if (!user_ptr_ok(argp, sizeof(struct k_termios))) {
            return -1;
        }
        task->termios = *(const struct k_termios *)argp;
        return 0;
    }
    if (request == TIOCGWINSZ) {
        if (!user_ptr_ok(argp, sizeof(struct k_winsize))) {
            return -1;
        }
        uint32_t cols, rows;
        /* No framebuffer console means the serial line is the only
         * terminal, and nothing tells the kernel how big the far end is
         * -- report the classic 80x24 rather than failing, so a
         * full-screen program still has a usable (if possibly wrong)
         * geometry to lay out against. */
        if (fbconsole_size(&cols, &rows) != 0) {
            cols = 80;
            rows = 24;
        }
        struct k_winsize *ws = argp;
        ws->ws_row = (uint16_t)rows;
        ws->ws_col = (uint16_t)cols;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    return -1;
}

static int64_t sys_getpid_impl(void) {
    struct process *p = process_current();
    return p != NULL ? p->pid : -1;
}

static int64_t sys_fork_impl(struct interrupt_frame *frame) {
    struct process *me = process_current();
    if (me == NULL) {
        return -1;
    }
    return process_fork(me, frame);
}

/* Simplified single-arg exec(path) -- no argv/envp yet. Resolves `path`
 * against the calling process' own cwd, reads the whole file (reusing
 * the same vnode->data/size access pattern as elf.c/blkfs.c), and hands
 * it to process_exec(). Never returns on success -- return_to_kernel()
 * abandons the old program's execution entirely, same as exit(). */
static int64_t sys_execve_impl(const char *path) {
    struct process *me = process_current();
    if (me == NULL || !user_ptr_ok(path, 1)) {
        return -1;
    }

    struct vnode *node = vfs_resolve(me->task.cwd, path);
    if (node == NULL || node->type != VNODE_FILE) {
        return -1;
    }

    if (process_exec(me, node->data, node->size) != 0) {
        return -1;
    }
    return_to_kernel(0);
}

/* Simplified waitpid(pid, status_ptr) -- no options, no WNOHANG. Blocks
 * (via an ordinary *recursive* call into scheduler_run_until(), running
 * other processes -- including the target, once picked -- until it
 * exits) rather than the timer-driven suspend/resume M13's Phase B
 * preemption uses; see process.h's own doc comment on why a plain
 * recursive call is sufficient and correct here. */
static int64_t sys_wait_impl(int pid, int *status_ptr) {
    struct process *me = process_current();
    if (me == NULL) {
        return -1;
    }

    struct process *target = process_find_child(me->pid, pid);
    if (target == NULL) {
        return -1;
    }

    int target_pid = target->pid;
    int status = scheduler_run_until(target_pid);
    if (status_ptr != NULL && user_ptr_ok(status_ptr, sizeof(int))) {
        *status_ptr = status;
    }
    return target_pid;
}

void syscall_dispatch(struct interrupt_frame *frame) {
    uint64_t number = frame->rax;
    int64_t result;

    switch (number) {
        case SYS_read:
            result = sys_read_impl((int)frame->rdi, (void *)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_write:
            result = sys_write_impl((int)frame->rdi, (const void *)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_open:
            result = sys_open_impl((const char *)frame->rdi, (int)frame->rsi);
            break;
        case SYS_close:
            result = sys_close_impl((int)frame->rdi);
            break;
        case SYS_ioctl:
            result = sys_ioctl_impl((int)frame->rdi, (uint64_t)frame->rsi, (void *)frame->rdx);
            break;
        case SYS_lseek:
            result = sys_lseek_impl((int)frame->rdi, (int64_t)frame->rsi, (int)frame->rdx);
            break;
        case SYS_chdir:
            result = sys_chdir_impl((const char *)frame->rdi);
            break;
        case SYS_mkdir:
            result = sys_mkdir_impl((const char *)frame->rdi);
            break;
        case SYS_getdents:
            result = sys_getdents_impl((int)frame->rdi, (struct dirent *)frame->rsi);
            break;
        case SYS_brk:
            result = sys_brk_impl((uint64_t)frame->rdi);
            break;
        case SYS_getpid:
            result = sys_getpid_impl();
            break;
        case SYS_fork:
            result = sys_fork_impl(frame);
            break;
        case SYS_execve:
            result = sys_execve_impl((const char *)frame->rdi);
            break; /* only reached on failure -- success never returns */
        case SYS_wait4:
            result = sys_wait_impl((int)frame->rdi, (int *)frame->rsi);
            break;
        case SYS_exit: {
            struct process *me = process_current();
            int code = (int)frame->rdi;
            kprintf("\nprocess %d exited with code %d\n", me != NULL ? me->pid : -1, code);
            if (me != NULL) {
                me->state = PROC_ZOMBIE;
                me->exit_status = code;
            }
            return_to_kernel(code);
        }
        default:
            result = -1;
            break;
    }

    frame->rax = (uint64_t)result;
}
