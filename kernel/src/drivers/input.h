#ifndef DRIVERS_INPUT_H
#define DRIVERS_INPUT_H

/* Every keyboard the kernel can read, behind one poll: the virtio-input
 * keyboard (QEMU) and COM1, and on a real PC whatever keyboard driver it
 * has. Sources that aren't present just report nothing, so this is safe
 * to call whatever got initialized.
 *
 * Non-blocking: returns the next byte, or -1 if none is pending. Keys
 * with no single byte come out as the escape sequences a VT100-style
 * terminal sends (ESC [ A for Up, ...), the same from every source. */
int input_poll_char(void);

#endif
