/* Test payload: proves tcgetattr()/tcsetattr() reach the real per-task
 * termios (see kernel/src/exec/syscall.c's sys_ioctl_impl()) and that
 * clearing ICANON/ECHO genuinely disables the kernel's line-buffering
 * and echo -- reads a few raw keystrokes one at a time (no Enter
 * needed, nothing echoed but this program's own explicit print), then
 * restores the original settings before exiting. Needs REAL
 * interactively-typed keystrokes to prove anything; a scripted
 * `\r`-terminated line doesn't exercise the raw-mode path at all. */

#include "libc.h"

int main(void) {
    struct termios orig;
    if (tcgetattr(0, &orig) != 0) {
        printf("tcgetattr failed\n");
        exit(1);
    }
    printf("default c_lflag: %x (ICANON|ECHO expected)\n", orig.c_lflag);

    struct termios raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw) != 0) {
        printf("tcsetattr(raw) failed\n");
        exit(1);
    }

    printf("raw mode on -- type 3 keys:\n");
    for (int i = 0; i < 3; i++) {
        char c;
        long n = read(0, &c, 1);
        if (n != 1) {
            printf("raw read failed\n");
            break;
        }
        printf("got byte: %x '%c'\n", (unsigned char)c, c);
    }

    if (tcsetattr(0, TCSANOW, &orig) != 0) {
        printf("tcsetattr(restore) failed\n");
        exit(1);
    }
    printf("termtest done\n");
    exit(0);
}
