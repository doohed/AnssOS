/* M11 test payload: proves brk()-backed malloc/free work -- several
 * allocations of varying sizes, a pattern-fill/verify pass through each,
 * a free()+reuse check, then a clean exit. */

#include "libc.h"

int main(void) {
    printf("malloc/free test\n");

    void *a = malloc(16);
    void *b = malloc(256);
    void *c = malloc(4096);
    printf("a=%p b=%p c=%p\n", a, b, c);

    if (a == NULL || b == NULL || c == NULL) {
        printf("allocation failed!\n");
        exit(1);
    }

    memset(a, 0xAA, 16);
    memset(b, 0xBB, 256);
    memset(c, 0xCC, 4096);

    int ok = 1;
    unsigned char *ba = a;
    unsigned char *bb = b;
    unsigned char *bc = c;
    for (int i = 0; i < 16; i++) {
        if (ba[i] != 0xAA) {
            ok = 0;
        }
    }
    for (int i = 0; i < 256; i++) {
        if (bb[i] != 0xBB) {
            ok = 0;
        }
    }
    for (int i = 0; i < 4096; i++) {
        if (bc[i] != 0xCC) {
            ok = 0;
        }
    }
    printf("pattern check: %s\n", ok ? "OK" : "FAIL");

    free(b);
    void *d = malloc(200); /* should reuse b's freed slot (first-fit) */
    printf("d=%p (reused freed slot: %s)\n", d, d == b ? "yes" : "no");

    free(a);
    free(c);
    free(d);

    printf("malloc test done\n");
    exit(ok ? 0 : 1);
}
