#ifndef DRIVERS_DISPLAY_H
#define DRIVERS_DISPLAY_H

#include <stdint.h>

/* Whatever the screen is, as something to draw into: BGRX8888, one
 * uint32_t per pixel, `pitch` pixels from one row to the next (wider than
 * `width` on a firmware framebuffer, whose rows are often padded).
 * Nothing drawn here is visible until a display_flush*() call. */
struct framebuffer {
    volatile uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
};

/* Picks the display: virtio-gpu when PCI enumeration found one (QEMU),
 * otherwise the firmware's GOP framebuffer that Limine hands over (a real
 * PC, where the graphics card's UEFI driver set it up before boot). The
 * GOP path draws into a RAM copy and copies the changed part to the
 * screen on flush, since reading video memory back over PCIe -- which
 * scrolling does -- is very slow. pci_enumerate() and the PMM must be up.
 * Returns 0 and fills *out on success, -1 if there's no screen at all. */
int display_init(struct framebuffer *out);

/* "virtio-gpu" or "GOP framebuffer"; NULL before a successful init. */
const char *display_name(void);

void display_flush(void);

/* Makes just this rectangle visible -- what a console that only changed
 * a few characters should call. Clipped to the screen. */
void display_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

#endif
