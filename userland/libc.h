#ifndef USERLAND_LIBC_H
#define USERLAND_LIBC_H

#include <stddef.h>

/* Raw syscall wrappers -- userland/syscalls.c. */
long write(int fd, const void *buf, unsigned long len);
long read(int fd, void *buf, unsigned long len);
void exit(int code) __attribute__((noreturn));
long brk(unsigned long new_end);
int open(const char *path, int flags);
int close(int fd);
long lseek(int fd, long offset, int whence);
int chdir(const char *path);
int mkdir(const char *path);
int fork(void);
int execve(const char *path, char *const argv[]);
int waitpid(int pid, int *status);
int getpid(void);
int getcwd(char *buf, unsigned long size);
long ioctl(int fd, unsigned long request, void *argp);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x40
#define O_TRUNC 0x200

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Linux's real struct termios layout/flag values -- see
 * kernel/src/drivers/tty.h for why. tcgetattr()/tcsetattr()
 * (userland/libc/termios.c) are the usual POSIX-shaped wrappers over
 * ioctl(fd, TCGETS/TCSETS, ...) above. */
#define NCCS 19
struct termios {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[NCCS];
};
#define ICANON 0x0002
#define ECHO 0x0008
#define VMIN 6
#define VTIME 5
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSANOW 0
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int optional_actions, const struct termios *t);

/* Terminal geometry, for a full-screen program that has to lay itself
 * out -- see kernel/src/drivers/tty.h. Answered from the framebuffer
 * console's glyph grid, or 80x24 on a boot with no virtio-gpu. */
#define TIOCGWINSZ 0x5413
struct winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};

/* A deliberately simplified getdents() -- see
 * kernel/src/exec/syscall.c's sys_getdents_impl() -- one entry per call,
 * not real Linux getdents64's batched-buffer ABI. d_name's size must
 * match fs/vfs.h's VFS_NAME_MAX. opendir()/readdir()/closedir()/
 * rewinddir() (userland/libc/dirent.c) are the usual POSIX-shaped
 * wrappers built on top. */
#define DT_DIR 4
#define DT_REG 8
struct dirent {
    unsigned char d_type;
    char d_name[64];
};
long getdents(int fd, struct dirent *out);

typedef struct {
    int fd;
} DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
void rewinddir(DIR *dirp);

/* userland/libc/string.c */
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* userland/libc/malloc.c -- grows the process's brk-managed heap on
 * demand; see kernel/src/exec/syscall.c's SYS_brk handler. */
void *malloc(size_t size);
void free(void *ptr);

/* userland/libc/stdio.c -- all write to fd 1 (stdout). Mirrors kprintf's
 * own minimal format-spec support (see drivers/serial.h): %s %c %d %u
 * %x %X %p %lx %lu %ld %%, no width/precision. */
void putchar(char c);
void puts(const char *s);
void printf(const char *fmt, ...);

#endif
