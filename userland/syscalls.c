/* The raw syscall wrappers -- thin `int $0x80` stubs over AnssOS's
 * syscall ABI (vector 0x80, Linux-numbered: see kernel/src/exec/
 * syscall.c). userland/libc/*.c builds the rest of the M11 libc
 * (malloc, printf, string.h) on top of these. */

#include "libc.h"

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

int chdir(const char *path) {
    return (int)syscall3(80, (long)(unsigned long)path, 0, 0);
}

int mkdir(const char *path) {
    return (int)syscall3(83, (long)(unsigned long)path, 0, 0);
}

/* fork()'s "returns twice" semantics fall out of this being an ordinary
 * function call with no special handling needed here at all: the kernel
 * builds the child's very first resume to look exactly like it's
 * returning from this same syscall3(57, ...) call too, with rax=0
 * already baked in (see kernel/src/exec/process.c's process_fork()) --
 * from this function's own perspective, it's just a normal syscall that
 * happens to be "returned into" twice, once for each process. */
int fork(void) {
    return (int)syscall3(57, 0, 0, 0);
}

int execve(const char *path, char *const argv[]) {
    return (int)syscall3(59, (long)(unsigned long)path, (long)(unsigned long)argv, 0);
}

int getcwd(char *buf, unsigned long size) {
    return (int)syscall3(79, (long)(unsigned long)buf, (long)size, 0);
}

int waitpid(int pid, int *status) {
    return (int)syscall3(61, pid, (long)(unsigned long)status, 0);
}

int getpid(void) {
    return (int)syscall3(39, 0, 0, 0);
}

long ioctl(int fd, unsigned long request, void *argp) {
    return syscall3(16, fd, request, (long)(unsigned long)argp);
}

long getdents(int fd, struct dirent *out) {
    return syscall3(217, fd, (long)(unsigned long)out, 0);
}

int audio_open(unsigned int rate_hz, unsigned int channels) {
    return (int)syscall3(900, rate_hz, channels, 0);
}

long audio_write(const void *buf, unsigned int len) {
    return syscall3(901, (long)(unsigned long)buf, len, 0);
}

int audio_close(void) {
    return (int)syscall3(902, 0, 0, 0);
}

int poll_key(void) {
    return (int)syscall3(903, 0, 0, 0);
}

int pipe(int pipefd[2]) {
    return (int)syscall3(22, (long)(unsigned long)pipefd, 0, 0);
}

int use_as_stdio(int stdin_fd, int stdout_fd) {
    return (int)syscall3(904, stdin_fd, stdout_fd, 0);
}
