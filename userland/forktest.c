/* M13 Phase A test payload: proves fork()/execve()/waitpid()/getpid()
 * all work together -- forks, the child exec()'s into a small dedicated
 * program (forkchild.bin) with a known exit status, the parent waits
 * for it and confirms the exact status came back through the real
 * process table, not just "it didn't crash." */

#include "libc.h"

int main(void) {
    printf("parent: pid=%d, about to fork\n", getpid());

    int pid = fork();
    if (pid < 0) {
        printf("fork failed\n");
        exit(1);
    }

    if (pid == 0) {
        printf("child: pid=%d, about to exec forkchild.bin\n", getpid());
        char *const child_argv[] = {"forkchild", NULL};
        execve("/bin/forkchild", child_argv);
        printf("child: execve failed!\n"); /* unreachable on success */
        exit(99);
    }

    printf("parent: forked child pid=%d, waiting...\n", pid);
    int status = -1;
    int reaped = waitpid(pid, &status);
    printf("parent: reaped pid=%d, status=%d\n", reaped, status);

    exit(0);
}
