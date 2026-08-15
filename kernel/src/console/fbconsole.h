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

/* Reports the console's character grid, i.e. what a full-screen program
 * needs from TIOCGWINSZ (see exec/syscall.c's sys_ioctl_impl()). Returns
 * -1 without touching the outputs if fbconsole_init() never ran -- a boot
 * with no virtio-gpu device, where the serial console is the only
 * terminal and the kernel has no idea how big it is. */
int fbconsole_size(uint32_t *out_cols, uint32_t *out_rows);

/* Suppresses the per-character virtio_gpu_flush() that
 * fbconsole_kprintf_sink() normally does, until the matching end_batch()
 * flushes once. A full-screen redraw is thousands of characters, and a
 * virtqueue round trip per character makes it unusably slow -- so a
 * whole write() from userland becomes exactly one flush. Nesting is not
 * supported (there's one flag); safe to call before fbconsole_init(). */
void fbconsole_begin_batch(void);
void fbconsole_end_batch(void);

/* Draws text at an arbitrary cell position without touching the scrolling
 * cursor -- used by console/splash.c to place a centered logo/animation
 * outside the normal line-by-line log flow. Does not flush to the host
 * display; call virtio_gpu_flush() when ready. fbconsole_init() must have
 * already run. */
void fbconsole_draw_text_at(uint32_t col, uint32_t row, const char *s);

/* Suitable for kprintf_set_sink() (see drivers/serial.h): draws the byte */
/* and flushes to the host display on every newline, so log lines appear */
/* live instead of only once a whole message is buffered. */
void fbconsole_kprintf_sink(char c);

#endif
