#include "virtio_blk.h"
#include "virtio.h"
#include "../pci.h"
#include "../serial.h"
#include "../../mm/heap.h"

#include <stddef.h>
#include <stdint.h>

#define VIRTIO_BLK_T_IN 0  /* Read. */
#define VIRTIO_BLK_T_OUT 1 /* Write. */

#define VIRTIO_BLK_S_OK 0

struct __attribute__((packed)) virtio_blk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

/* Layout mandated by the virtio spec ("Block Device") -- we only need
 * the first field. */
struct __attribute__((packed)) virtio_blk_config {
    uint64_t capacity; /* In 512-byte sectors. */
};

static struct virtio_device vdev;
static struct virtio_queue requestq;
static uint64_t capacity_sectors;
static int initialized;

/* Scratch buffers for the header/status of every request -- kmalloc'd
 * once (heap memory is HHDM-backed, safe to hand to
 * virtio_queue_submit_chain(), see struct virtio_buffer's doc comment in
 * virtio.h) and reused, since requests are synchronous/one at a time. */
static struct virtio_blk_req_header *req_hdr;
static uint8_t *req_status;

int virtio_blk_init(void) {
    const struct pci_device *pci =
        pci_find_device(VIRTIO_BLK_PCI_VENDOR_ID, VIRTIO_BLK_PCI_DEVICE_ID);
    if (pci == NULL) {
        kprintf("virtio-blk: no block device found -- boot QEMU with -device virtio-blk-pci\n");
        return -1;
    }

    if (virtio_pci_init(pci, &vdev) != 0 || virtio_negotiate_features(&vdev, 0) != 0 ||
        virtio_queue_init(&vdev, 0, &requestq) != 0) {
        return -1;
    }
    virtio_driver_ok(&vdev);

    req_hdr = kmalloc(sizeof(struct virtio_blk_req_header));
    req_status = kmalloc(1);
    if (req_hdr == NULL || req_status == NULL) {
        kprintf("virtio-blk: out of memory\n");
        return -1;
    }

    volatile struct virtio_blk_config *cfg = (volatile struct virtio_blk_config *)vdev.device_cfg;
    capacity_sectors = cfg->capacity;

    initialized = 1;
    kprintf("virtio-blk: ready, %lu sectors (%lu MiB)\n", capacity_sectors,
            (capacity_sectors * VIRTIO_BLK_SECTOR_SIZE) / (1024 * 1024));
    return 0;
}

static int do_request(uint32_t type, uint64_t sector, void *data, uint32_t bytes) {
    if (!initialized) {
        return -1;
    }

    req_hdr->type = type;
    req_hdr->reserved = 0;
    req_hdr->sector = sector;
    *req_status = 0xFF;

    struct virtio_buffer bufs[3] = {
        {.addr = req_hdr, .len = sizeof(*req_hdr), .device_writable = 0},
        {.addr = data, .len = bytes, .device_writable = (type == VIRTIO_BLK_T_IN)},
        {.addr = req_status, .len = 1, .device_writable = 1},
    };
    virtio_queue_submit_chain(&requestq, bufs, 3);
    virtio_queue_wait(&requestq);

    return (*req_status == VIRTIO_BLK_S_OK) ? 0 : -1;
}

int virtio_blk_is_ready(void) {
    return initialized;
}

uint64_t virtio_blk_capacity_sectors(void) {
    return capacity_sectors;
}

int virtio_blk_read(uint64_t sector, void *buf, uint32_t sector_count) {
    return do_request(VIRTIO_BLK_T_IN, sector, buf, sector_count * VIRTIO_BLK_SECTOR_SIZE);
}

int virtio_blk_write(uint64_t sector, const void *data, uint32_t sector_count) {
    return do_request(VIRTIO_BLK_T_OUT, sector, (void *)data,
                      sector_count * VIRTIO_BLK_SECTOR_SIZE);
}
