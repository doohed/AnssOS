/* The 3-function "libc" these test payloads link against -- thin
 * wrappers over AnssOS's syscall ABI (vector 0x80, Linux-numbered:
 * see kernel/src/exec/syscall.c). Not a real libc; just enough to prove
 * the M10 pipeline end-to-end. */

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
