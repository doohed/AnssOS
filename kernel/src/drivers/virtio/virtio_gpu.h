#ifndef DRIVERS_VIRTIO_VIRTIO_GPU_H
#define DRIVERS_VIRTIO_VIRTIO_GPU_H

#include "../display.h"

#include <stdint.h>

#define VIRTIO_GPU_PCI_VENDOR_ID 0x1af4
#define VIRTIO_GPU_PCI_DEVICE_ID 0x1050

/* Finds the virtio-gpu-pci device via PCI (pci_enumerate() must have run */
/* already), completes the virtio init handshake, creates a 2D resource */
/* sized to the display's preferred mode (falling back to 1024x768), */
/* attaches a PMM-backed linear buffer to it, and sets it as scanout 0. */
/* Returns 0 on success and fills *out_fb with a pointer usable for */
/* direct pixel writes (HHDM-mapped, pitch == width). Normally called */
/* through display_init() rather than directly. */
int virtio_gpu_init(struct framebuffer *out_fb);

/* Pushes the whole framebuffer to the host display: TRANSFER_TO_HOST_2D */
/* followed by RESOURCE_FLUSH, both covering the full resource rect. Call */
/* this after writing pixels for them to actually become visible. */
void virtio_gpu_flush(void);

#endif
