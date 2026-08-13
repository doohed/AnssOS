/* Thin ioctl(TCGETS/TCSETS) wrappers -- see kernel/src/exec/syscall.c's
 * sys_ioctl_impl(). */

#include "../libc.h"

int tcgetattr(int fd, struct termios *t) {
    return (int)ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int optional_actions, const struct termios *t) {
    (void)optional_actions; /* Nothing to flush/drain -- there's no output buffering here. */
    return (int)ioctl(fd, TCSETS, (void *)t);
}
