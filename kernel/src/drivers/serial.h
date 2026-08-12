#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

/* COM1 driver, used both as a debug console and as the backing device for */
/* kprintf until we have a graphical console (see console/fbconsole.*). */

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

__attribute__((format(printf, 1, 2))) void kprintf(const char *fmt, ...);

/* Registers a second output for everything kprintf() prints (e.g. the */
/* framebuffer console, once it's up) -- pass NULL to unregister. */
void kprintf_set_sink(void (*sink)(char c));

#endif
