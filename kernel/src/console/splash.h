#ifndef CONSOLE_SPLASH_H
#define CONSOLE_SPLASH_H

#include "../drivers/virtio/virtio_gpu.h"

/* Draws a centered ASCII-art logo with a small looping "." / ".." / "..."
 * activity indicator beneath it, in the spirit of what Fedora/Windows show
 * while their real init runs in the background. fbconsole_init() must
 * have already run. Blocks for a few animation cycles -- pacing is a
 * plain busy-wait spin, since there's no timer interrupt yet -- then
 * returns with the logo still on screen; the caller clears it (e.g. via
 * fbconsole_clear()) when ready to show the real console. */
void splash_show(struct virtio_gpu_fb *fb);

#endif
