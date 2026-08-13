#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

/* COM1 driver, used both as a debug console and as the backing device for */
/* kprintf until we have a graphical console (see console/fbconsole.*). */

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

/* Non-blocking: returns the next byte received on COM1, or -1 if none is
 * pending. A second input source alongside the virtio-input keyboard
 * (see drivers/virtio/virtio_input.h) -- useful because virtio-input
 * needs a real graphical window with keyboard focus to receive anything
 * at all, which a headless/terminal-only setup (`-display none`, the
 * default in scripts/run-qemu.sh) doesn't have. QEMU's `-serial stdio`
 * wires this straight to the host terminal, so typing there reaches the
 * shell like a normal serial console. */
int serial_poll_char(void);

__attribute__((format(printf, 1, 2))) void kprintf(const char *fmt, ...);

/* Registers a second output for everything kprintf() prints (e.g. the */
/* framebuffer console, once it's up) -- pass NULL to unregister. */
void kprintf_set_sink(void (*sink)(char c));

#endif
