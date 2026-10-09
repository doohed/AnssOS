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

/* Linux's real TIOCGWINSZ request value and struct winsize layout, same
 * free-compat reasoning as everything else here. A full-screen program
 * (scarf, userland/rust/scarf/) can't lay anything out without knowing how big the
 * terminal is -- see exec/syscall.c's sys_ioctl_impl(), which answers
 * from console/fbconsole.c's actual glyph grid when the framebuffer
 * console is up. ws_xpixel/ws_ypixel are reported as 0, which is what
 * real terminals that don't know their pixel size do. */
#define TIOCGWINSZ 0x5413

/* TIOCSWINSZ sets the calling process's own window size (stored in its
 * struct usertask, inherited by fork() and kept across exec()), which
 * TIOCGWINSZ then reports instead of the physical console's. On Linux
 * this lives on the pty; AnssOS has no ptys, so it lives on the process.
 * It's how tile (userland/rust/tile/) tells each pane's programs how big
 * their pane is -- set once in the pane's child before it execs `sh`,
 * then inherited by everything that `sh` runs. */
#define TIOCSWINSZ 0x5414

struct k_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

#endif
