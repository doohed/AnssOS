#ifndef DRIVERS_TTY_H
#define DRIVERS_TTY_H

#include <stdint.h>

/* Linux's real struct termios layout and flag/index values -- reused for
 * free future compatibility (same reasoning as this project's syscall
 * numbers, see exec/syscall.c), even though only a small slice of what
 * real termios describes is actually honored: ICANON/ECHO in c_lflag,
 * and VMIN/VTIME in c_cc[] (see exec/syscall.c's sys_read_impl()).
 * Everything else (baud rates, IXON/ISIG flow control, ...) is stored
 * and read back faithfully by TCGETS/TCSETS but otherwise a no-op. */
#define NCCS 19

struct k_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[NCCS];
};

#define ICANON 0x0002
#define ECHO 0x0008

#define VMIN 6
#define VTIME 5

#endif
