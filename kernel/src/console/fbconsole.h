#ifndef CONSOLE_FBCONSOLE_H
#define CONSOLE_FBCONSOLE_H

#include "../drivers/virtio/virtio_gpu.h"

/* Small scrolling text console drawn with the font8x8_basic bitmap font */
/* over a virtio_gpu_fb (see drivers/virtio/virtio_gpu.h). Flushes to the */
/* host display (virtio_gpu_flush()) after every write. */

void fbconsole_init(struct virtio_gpu_fb *fb);
void fbconsole_clear(void);
void fbconsole_putc(char c);
void fbconsole_write(const char *s);

/* Suitable for kprintf_set_sink() (see drivers/serial.h): draws the byte */
/* and flushes to the host display on every newline, so log lines appear */
/* live instead of only once a whole message is buffered. */
void fbconsole_kprintf_sink(char c);

#endif
