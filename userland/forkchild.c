/* The program forktest.bin exec()'s its forked child into -- a small,
 * dedicated target so forktest.c's parent can verify an exact, known
 * exit status came back through waitpid(). */

#include "libc.h"

int main(void) {
    printf("forkchild: pid=%d, hello from exec()'d child!\n", getpid());
    exit(7);
}
