/* M10 isolation test payload: deliberately faults (null-pointer write)
 * to prove a user-mode crash ends just this task, not the kernel -- see
 * idt.c's non-fatal user-mode-fault path. */

long write(int fd, const void *buf, unsigned long len);
void exit(int code) __attribute__((noreturn));

int main(void) {
    const char *msg = "about to write through a null pointer...\n";
    unsigned long len = 0;
    while (msg[len] != '\0') {
        len++;
    }
    write(1, msg, len);

    volatile int *bad = (volatile int *)0;
    *bad = 1; /* deliberate fault */

    exit(0); /* unreachable */
}
