/* M13 Phase B test payload: proves *preemptive* scheduling actually
 * works, not just fork()/exec()/wait(). Forks two children that each
 * spin in a tight busy loop -- no syscalls, no voluntary yield point at
 * all -- printing their own tag ('A'/'B') at intervals. With no
 * preemption, one child would always run to completion before the other
 * ever starts, so the output would read "AAAAAAAAAA" then "BBBBBBBBBB"
 * as two clean blocks. If the timer can genuinely interrupt a running
 * process mid-flight and hand the CPU to the other one, the tags
 * visibly interleave instead -- that's the actual, observable proof. */

#include "libc.h"

static void run_child(const char *tag) {
    for (long i = 0; i < 8000000; i++) {
        if (i % 400000 == 0) {
            printf("%s", tag);
        }
    }
    exit(0);
}

int main(void) {
    printf("preempttest: forking two busy-loop children (A and B)\n");

    int pid1 = fork();
    if (pid1 == 0) {
        run_child("A");
    }

    int pid2 = fork();
    if (pid2 == 0) {
        run_child("B");
    }

    waitpid(pid1, NULL);
    waitpid(pid2, NULL);

    printf("\npreempttest: both children finished\n");
    exit(0);
}
