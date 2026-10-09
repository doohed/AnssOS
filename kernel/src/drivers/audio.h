#ifndef DRIVERS_AUDIO_H
#define DRIVERS_AUDIO_H

#include <stdint.h>

/* Sound output, whichever device there is: virtio-sound under QEMU
 * (drivers/virtio/virtio_snd.h), Intel HD Audio on a real PC
 * (drivers/hda.h). Both take the same thing: S16LE PCM at 44100 or 48000
 * Hz, 1 or 2 channels, with audio_write() blocking while the device's
 * buffer is full -- which is what paces a player to real time. */

/* Picks the device. Returns 0 if there's one. */
int audio_init(void);

/* "virtio-sound", "HD Audio", or NULL. */
const char *audio_name(void);

int audio_open(uint32_t rate_hz, uint8_t channels);
int audio_write(const void *pcm_s16le, uint32_t bytes);
int audio_close(void);

#endif
