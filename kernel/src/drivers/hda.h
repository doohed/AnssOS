#ifndef DRIVERS_HDA_H
#define DRIVERS_HDA_H

#include <stdint.h>

/* Intel High Definition Audio: the onboard sound of essentially every PC
 * since 2004 (QEMU: -device intel-hda -device hda-output). Playback only,
 * through the codec's analog outputs -- line out, headphones, speakers.
 * HDMI/DisplayPort audio from a graphics card is skipped: the GPU's
 * display engine has to be told to carry audio, which needs a real GPU
 * driver. Polled, like every other driver here.
 *
 * Same contract as drivers/virtio/virtio_snd.h, which drivers/audio.c
 * picks between: S16LE only, 44100 or 48000 Hz, 1 or 2 channels (mono is
 * played on both sides). */

/* Probes every HD Audio controller (PCI class 04.03) and keeps the first
 * whose codec has an analog output it can route. pci_enumerate() and
 * the timer must be up. Returns 0 if there's somewhere to play. */
int hda_init(void);

int hda_open(uint32_t rate_hz, uint8_t channels);

/* Copies PCM into the DMA ring, blocking while it's full -- which paces
 * the caller to real time. Returns `bytes`, or -1 if the stream isn't
 * open or the hardware stopped moving. */
int hda_write(const void *pcm_s16le, uint32_t bytes);

/* Lets what's queued finish playing, then stops the stream. */
int hda_close(void);

#endif
