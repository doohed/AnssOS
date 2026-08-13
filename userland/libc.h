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
int execve(const char *path);
int waitpid(int pid, int *status);
int getpid(void);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x40

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

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
