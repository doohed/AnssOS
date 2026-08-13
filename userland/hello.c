/* M10 happy-path test payload: proves write(), read(), and a clean
 * exit() all round-trip correctly through AnssOS's ring-3 pipeline. */

long write(int fd, const void *buf, unsigned long len);
long read(int fd, void *buf, unsigned long len);
void exit(int code) __attribute__((noreturn));

static unsigned long strlen_(const char *s) {
    unsigned long n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int main(void) {
    const char *greeting = "Hello from ring 3! AnssOS userspace is alive.\nType something: ";
    write(1, greeting, strlen_(greeting));

    char buf[64];
    long n = read(0, buf, sizeof(buf));
    if (n < 0) {
        n = 0;
    }

    const char *echo_prefix = "You typed: ";
    write(1, echo_prefix, strlen_(echo_prefix));
    write(1, buf, (unsigned long)n);
    write(1, "\n", 1);

    exit(42);
}
