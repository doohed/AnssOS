#ifndef DRIVERS_VIRTIO_VIRTIO_SND_H
#define DRIVERS_VIRTIO_VIRTIO_SND_H

#include <stdint.h>

#define VIRTIO_SND_PCI_VENDOR_ID 0x1af4
#define VIRTIO_SND_PCI_DEVICE_ID 0x1059

/* Finds the virtio-sound-pci device via PCI (pci_enumerate() must have run
 * already), completes the virtio init handshake, sets up the control and
 * tx virtqueues (the event/rx queues go untouched -- this driver only ever
 * plays audio, same "only wire up the queues you use" precedent
 * virtio_gpu.c already sets by skipping its cursor queue), and confirms
 * stream 0 is an output stream via PCM_INFO. Returns 0 on success, -1 if
 * no virtio-sound device is present (non-fatal -- callers should skip
 * audio the same way a missing virtio-blk/virtio-gpu device is handled). */
int virtio_snd_init(void);

int virtio_snd_is_ready(void);

/* Configures stream 0 (VIRTIO_SND_R_PCM_SET_PARAMS + PREPARE + START).
 * Only 16-bit signed little-endian PCM is supported; rate_hz must be
 * 44100 or 48000, channels must be 1 or 2 -- the one concrete
 * combination this driver's tx path assumes. Returns 0 on success, -1 on
 * an unsupported format or a device-reported error. */
int virtio_snd_open(uint32_t rate_hz, uint8_t channels);

/* Sends `bytes` of S16LE PCM to stream 0, chunked internally at
 * PERIOD_BYTES per virtqueue transfer, blocking (via virtio_queue_wait())
 * until each chunk is consumed -- this is what paces playback to real
 * time on a real audio backend. Returns bytes sent, or -1 on failure /
 * if the stream isn't open. */
int virtio_snd_write(const void *pcm_s16le, uint32_t bytes);

/* PCM_STOP + PCM_RELEASE on stream 0. Returns 0 on success, -1 if the
 * stream wasn't open or the device reports an error. */
int virtio_snd_close(void);

#endif
