/* The raw syscall wrappers -- thin `int $0x80` stubs over AnssOS's
 * syscall ABI (vector 0x80, Linux-numbered: see kernel/src/exec/
 * syscall.c). userland/libc/*.c builds the rest of the M11 libc
 * (malloc, printf, string.h) on top of these. */

static long syscall3(long number, long a1, long a2, long a3) {
    long ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(number), "D"(a1), "S"(a2), "d"(a3) : "memory");
    return ret;
}

long write(int fd, const void *buf, unsigned long len) {
    return syscall3(1, fd, (long)(unsigned long)buf, (long)len);
}

long read(int fd, void *buf, unsigned long len) {
    return syscall3(0, fd, (long)(unsigned long)buf, (long)len);
}

void exit(int code) {
    syscall3(60, code, 0, 0);
    for (;;) {
    }
}

long brk(unsigned long new_end) {
    return syscall3(12, (long)new_end, 0, 0);
}

int open(const char *path, int flags) {
    return (int)syscall3(2, (long)(unsigned long)path, flags, 0);
}

int close(int fd) {
    return (int)syscall3(3, fd, 0, 0);
}

long lseek(int fd, long offset, int whence) {
    return syscall3(8, fd, offset, whence);
}
