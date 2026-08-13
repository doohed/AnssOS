/* M10 isolation test payload: deliberately faults (null-pointer write)
 * to prove a user-mode crash ends just this task, not the kernel -- see
 * idt.c's non-fatal user-mode-fault path. */

#include "libc.h"

int main(void) {
    const char *msg = "about to write through a null pointer...\n";
    write(1, msg, strlen(msg));

    volatile int *bad = (volatile int *)0;
    *bad = 1; /* deliberate fault */

    exit(0); /* unreachable */
}
