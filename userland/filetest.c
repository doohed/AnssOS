/* M11 test payload: proves open()/read()/write()/lseek()/close() reach
 * the real VFS, not a copy -- reads a known fixture file (written by
 * main.c's self-test wiring at boot, see kernel/src/main.c), prints it,
 * then modifies it in place and appends to it so a follow-up `cat` from
 * the shell can confirm the change actually landed. */

#include "libc.h"

int main(void) {
    const char *path = "/filetest.txt";

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("open(%s, O_RDONLY) failed\n", path);
        exit(1);
    }
    char buf[128];
    long n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        printf("read failed\n");
        exit(1);
    }
    buf[n] = '\0';
    printf("read %d bytes: %s\n", (int)n, buf);
    close(fd);

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        printf("open(%s, O_WRONLY) failed\n", path);
        exit(1);
    }
    const char *patch = "PATCHED";
    long w = write(fd, patch, strlen(patch));
    printf("wrote %d bytes at offset 0\n", (int)w);

    lseek(fd, 0, SEEK_END);
    const char *tail = " [appended]";
    write(fd, tail, strlen(tail));
    close(fd);

    printf("filetest done\n");
    exit(0);
}
