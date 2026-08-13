#include "syscall.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/usermode.h"
#include "../drivers/serial.h"
#include "../drivers/virtio/virtio_input.h"

#include <stddef.h>
#include <stdint.h>

/* Linux's syscall numbers for this initial handful -- reusing them costs
 * nothing now and avoids a renumbering exercise later if AnssOS ever
 * wants to run real static Linux binaries (a distant aspiration, but a
 * free one to keep open by not inventing our own numbering here). */
#define SYS_read 0
#define SYS_write 1
#define SYS_exit 60

/* Coarse user-pointer validation: real hardware/OS-grade safety needs a
 * fault-recovering copy_from_user(); a bad-but-in-range user pointer
 * still just crashes that one task (see idt.c's non-fatal user-fault
 * path) rather than the kernel, so this first pass only needs to reject
 * pointers that would reach into kernel space. */
static int user_ptr_ok(const void *ptr, size_t len) {
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    return addr < 0x0000800000000000ull && addr + len < 0x0000800000000000ull;
}

static int64_t sys_write_impl(int fd, const void *buf, size_t len) {
    if ((fd != 1 && fd != 2) || !user_ptr_ok(buf, len)) {
        return -1;
    }
    const char *bytes = buf;
    for (size_t i = 0; i < len; i++) {
        kprintf("%c", bytes[i]);
    }
    return (int64_t)len;
}

/* Line-buffered, matching the shell's own read_line() -- polls both
 * input sources, echoes as it goes, stops at Enter or `len` bytes. Raw
 * (unbuffered/non-canonical) termios-style reads are future work. */
static int64_t sys_read_impl(int fd, void *buf, size_t len) {
    if (fd != 0 || !user_ptr_ok(buf, len)) {
        return -1;
    }
    char *bytes = buf;
    size_t n = 0;
    while (n < len) {
        int c = virtio_input_poll_char();
        if (c < 0) {
            c = serial_poll_char();
        }
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
        case SYS_exit:
            kprintf("\nuser program exited with code %d\n", (int)frame->rdi);
            return_to_kernel((int)frame->rdi);
        default:
            result = -1;
            break;
    }

    frame->rax = (uint64_t)result;
}
