/* Test payload: proves mkdir()/chdir()/O_CREAT work together -- creates
 * a directory, moves into it, creates a brand new file there via a
 * *relative* path (proving cwd resolution), writes to it, then reopens
 * and reads it back. A follow-up `ls`/`cat` from the shell (see
 * kernel/src/main.c's self-test wiring and the M11-follow-up plan)
 * confirms the directory/file landed in the real VFS tree, not a
 * private copy. */

#include "libc.h"

int main(void) {
    if (mkdir("/dirtest_dir") != 0) {
        printf("mkdir failed\n");
        exit(1);
    }
    printf("mkdir /dirtest_dir: OK\n");

    if (chdir("/dirtest_dir") != 0) {
        printf("chdir failed\n");
        exit(1);
    }
    printf("chdir /dirtest_dir: OK\n");

    int fd = open("newfile.txt", O_WRONLY | O_CREAT);
    if (fd < 0) {
        printf("open(newfile.txt, O_WRONLY|O_CREAT) failed\n");
        exit(1);
    }
    const char *msg = "created via O_CREAT + a relative path\n";
    write(fd, msg, strlen(msg));
    close(fd);
    printf("created and wrote newfile.txt\n");

    fd = open("newfile.txt", O_RDONLY);
    if (fd < 0) {
        printf("reopen for read failed\n");
        exit(1);
    }
    char buf[64];
    long n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        n = 0;
    }
    buf[n] = '\0';
    close(fd);
    printf("read back: %s", buf);

    exit(0);
}
