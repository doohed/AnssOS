/* M19 test payload: proves pipe()/use_as_stdio() work before anything
 * (userland/sh.c, userland/tile.c) depends on them.
 *
 * Three checks: a basic write-then-read round trip through one pipe; the
 * non-blocking contract pipes are built around (empty-but-open reads as
 * 0, not a block -- see kernel/src/exec/pipe.h's own comment on why
 * blocking would deadlock this kernel's ring-3-only preemption; closed
 * reads as -1, real EOF); and the actual usage pattern tile.c will need
 * -- fork() a child, redirect its stdio to pipes with use_as_stdio()
 * *before* execve(), and read its captured output back after it exits. */

#include "libc.h"

int main(void) {
    printf("pipetest: pid=%d\n", getpid());
    char buf[128];

    /* --- 1: basic round trip --- */
    int fds[2];
    if (pipe(fds) != 0) {
        printf("pipe() failed\n");
        exit(1);
    }
    const char *msg = "hello through a pipe";
    long wn = write(fds[1], msg, strlen(msg));
    if (wn != (long)strlen(msg)) {
        printf("FAIL: pipe write returned %ld, expected %d\n", wn, (int)strlen(msg));
        exit(1);
    }
    long rn = read(fds[0], buf, sizeof(buf) - 1);
    if (rn < 0) {
        printf("FAIL: pipe read failed\n");
        exit(1);
    }
    buf[rn] = '\0';
    if (strcmp(buf, msg) != 0) {
        printf("FAIL: roundtrip mismatch, got '%s'\n", buf);
        exit(1);
    }
    printf("OK: basic pipe roundtrip ('%s')\n", buf);
    close(fds[0]);
    close(fds[1]);

    /* --- 2: non-blocking contract --- */
    int fds2[2];
    pipe(fds2);
    long n2 = read(fds2[0], buf, 1);
    if (n2 != 0) {
        printf("FAIL: expected 0 (no data yet) from an empty open pipe, got %ld\n", n2);
        exit(1);
    }
    printf("OK: empty-but-open pipe read returns 0, not a block\n");
    close(fds2[1]); /* Close the write end -- now it should read as EOF. */
    long n3 = read(fds2[0], buf, 1);
    if (n3 != -1) {
        printf("FAIL: expected -1 (EOF) once the write end closed, got %ld\n", n3);
        exit(1);
    }
    printf("OK: closed write end reads as -1 (EOF)\n");
    close(fds2[0]);

    /* --- 3: the real pattern -- redirect a child's stdio, exec it,
     * capture its output, confirm EOF shows up once it exits. --- */
    int in[2], out[2];
    pipe(in);
    pipe(out);
    int pid = fork();
    if (pid < 0) {
        printf("FAIL: fork failed\n");
        exit(1);
    }
    if (pid == 0) {
        close(in[1]);
        close(out[0]);
        use_as_stdio(in[0], out[1]);
        char *const child_argv[] = {"forkchild", NULL};
        execve("/bin/forkchild", child_argv);
        exit(99); /* unreachable on success */
    }
    close(in[0]);
    close(out[1]);

    int status = -1;
    waitpid(pid, &status); /* Reaps the child -- process exit already
                             * closed its end of `out` (process_close_
                             * stdio_pipes()), so draining below will
                             * see real EOF, not just "quiet for now". */

    long total = 0;
    int saw_eof = 0;
    while (total < (long)sizeof(buf) - 1) {
        long n = read(out[0], buf + total, sizeof(buf) - 1 - total);
        if (n > 0) {
            total += n;
            continue;
        }
        if (n == -1) {
            saw_eof = 1;
        }
        break;
    }
    buf[total] = '\0';
    if (!saw_eof) {
        printf("FAIL: never saw EOF on the child's captured stdout\n");
        exit(1);
    }
    printf("OK: captured from exec()'d child (status=%d): %s", status, buf);
    close(in[1]);
    close(out[0]);

    printf("pipetest done\n");
    exit(0);
}
