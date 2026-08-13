#ifndef SHELL_SHELL_H
#define SHELL_SHELL_H

/* Interactive command shell: reads lines from the virtio-input keyboard
 * (virtio_input_poll_char()), echoes to the framebuffer console + serial
 * via kprintf, and dispatches to a small set of builtins (see shell.c).
 * Command names are matched case-insensitively ("HELP"/"Help"/"help" are
 * all the same command); arguments after the command word are passed
 * through exactly as typed. Never returns. */
void shell_run(void);

#endif
