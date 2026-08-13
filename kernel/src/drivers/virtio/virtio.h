#ifndef DRIVERS_VIRTIO_VIRTIO_H
#define DRIVERS_VIRTIO_VIRTIO_H

#include "../pci.h"

#include <stdint.h>

/* Generic virtio 1.x ("modern") PCI transport: capability parsing, device */
/* status/feature negotiation, and split virtqueues, polling-only (no */
/* IRQ/MSI-X yet). Device-specific drivers (virtio_gpu.c, and later */
/* virtio_blk/virtio_net) build on this instead of touching PCI config */
/* space or virtqueue layout directly. */

#define VIRTIO_F_VERSION_1 32 /* Feature bit; required to drive as "modern". */

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER 0x02
#define VIRTIO_STATUS_DRIVER_OK 0x04
#define VIRTIO_STATUS_FEATURES_OK 0x08
#define VIRTIO_STATUS_NEEDS_RESET 0x40
#define VIRTIO_STATUS_FAILED 0x80

#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2

struct __attribute__((packed)) virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
};

struct virtio_queue {
    uint16_t index;
    uint16_t size;
    volatile struct virtq_desc *desc;
    volatile struct virtq_avail *avail;
    volatile struct virtq_used *used;
    uint16_t free_head;
    uint16_t num_free;
    uint16_t last_used_idx;
    volatile uint16_t *notify;
};

/* Opaque to everyone but virtio.c -- callers only ever touch it through */
/* struct virtio_device.common as a pointer. */
struct virtio_pci_common_cfg;

struct virtio_device {
    struct pci_device pci;
    volatile struct virtio_pci_common_cfg *common;
    volatile uint8_t *notify_base;
    uint32_t notify_off_multiplier;
    volatile uint8_t *isr;
    volatile void *device_cfg; /* Device-specific config (e.g. virtio_gpu_config). */
};

/* Locates and maps a device's virtio-pci capabilities via the PCI */
/* capability list and the HHDM, enables Memory Space + Bus Master in the */
/* PCI command register, and resets the device (spec init step 1). */
/* Returns 0 on success. */
int virtio_pci_init(const struct pci_device *pci, struct virtio_device *vdev);

/* Runs spec init steps 2-5: ACKNOWLEDGE, DRIVER, feature negotiation */
/* (VIRTIO_F_VERSION_1 is always requested in addition to `wanted`), */
/* FEATURES_OK. Returns 0 on success, -1 if the device doesn't support */
/* VIRTIO_F_VERSION_1 or rejects FEATURES_OK. */
int virtio_negotiate_features(struct virtio_device *vdev, uint64_t wanted);

/* Sets DRIVER_OK (spec init step 8) -- call once every queue you need is */
/* set up. */
void virtio_driver_ok(struct virtio_device *vdev);

/* Selects queue `index`, allocates one PMM page each for its descriptor */
/* table / avail ring / used ring (which clamps queue size to 256 */
/* descriptors -- comfortably more than virtio-gpu's control queue uses), */
/* and enables it. Returns 0 on success. */
int virtio_queue_init(struct virtio_device *vdev, uint16_t index, struct virtio_queue *q);

struct virtio_buffer {
    /* Must be an HHDM-mapped virtual pointer -- i.e. phys + hhdm_offset */
    /* for memory obtained from the PMM. The device DMAs by physical */
    /* address, so kernel .data/.bss/stack pointers (which live in the */
    /* higher-half link region, not the HHDM) will NOT work here; */
    /* virtio_queue_submit_chain() derives the physical address by */
    /* subtracting hhdm_offset back out. */
    void *addr;
    uint32_t len;
    int device_writable;
};

/* Chains up to 8 buffers into one descriptor list, publishes it on the */
/* avail ring, and kicks the device. */
void virtio_queue_submit_chain(struct virtio_queue *q, const struct virtio_buffer *buffers,
                               int count);

/* Polls until the device advances the used ring, reclaims the completed */
/* chain's descriptors, and returns the byte length the device wrote. */
uint32_t virtio_queue_wait(struct virtio_queue *q);

/* Non-blocking version of virtio_queue_wait(): if the used ring has
 * advanced, reclaims the descriptor chain, fills *out_desc_id (the head
 * descriptor's index -- useful when, unlike virtio_gpu.c, the caller
 * keeps a separate buffer per descriptor slot and needs to know which
 * one just completed, e.g. virtio_input.c's event queue) and *out_len,
 * and returns 1. Returns 0 immediately if nothing is ready yet. */
int virtio_queue_try_wait(struct virtio_queue *q, uint16_t *out_desc_id, uint32_t *out_len);

#endif
