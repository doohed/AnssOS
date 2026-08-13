#include "virtio.h"
#include "../serial.h"
#include "../../boot/requests.h"
#include "../../lib/string.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"

#include <stddef.h>
#include <stdint.h>

#define PCI_CAP_ID_VENDOR 0x09
#define PCI_STATUS_CAP_LIST 0x10
#define PCI_COMMAND_OFFSET 0x04
#define PCI_STATUS_OFFSET 0x06
#define PCI_CAP_PTR_OFFSET 0x34
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_BUS_MASTER 0x0004

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG 3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

/* Layout mandated by the virtio 1.x spec ("Common configuration */
/* structure layout"). All fields already fall on their natural */
/* alignment in this order, so no explicit packing is required. */
struct virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;

    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
};

static uint64_t bar_base(const struct pci_device *pci, int bar_index) {
    uint32_t lo = pci->bar[bar_index];
    if (lo & 0x1) {
        return 0; /* I/O space BAR -- modern virtio only uses MMIO BARs. */
    }
    uint64_t base = lo & 0xFFFFFFF0u;
    if (((lo >> 1) & 0x3) == 0x2) { /* 64-bit BAR: high half lives in the next slot. */
        base |= ((uint64_t)pci->bar[bar_index + 1]) << 32;
    }
    return base;
}

int virtio_pci_init(const struct pci_device *pci, struct virtio_device *vdev) {
    vdev->pci = *pci;
    vdev->common = NULL;
    vdev->notify_base = NULL;
    vdev->notify_off_multiplier = 0;
    vdev->isr = NULL;
    vdev->device_cfg = NULL;

    uint16_t pci_status = pci_config_read16(pci->bus, pci->slot, pci->func, PCI_STATUS_OFFSET);
    if (!(pci_status & PCI_STATUS_CAP_LIST)) {
        kprintf("virtio: device has no PCI capability list\n");
        return -1;
    }

    uint16_t command = pci_config_read16(pci->bus, pci->slot, pci->func, PCI_COMMAND_OFFSET);
    pci_config_write16(pci->bus, pci->slot, pci->func, PCI_COMMAND_OFFSET,
                       (uint16_t)(command | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER));

    uint8_t cap_ptr =
        (uint8_t)(pci_config_read8(pci->bus, pci->slot, pci->func, PCI_CAP_PTR_OFFSET) & 0xFC);
    while (cap_ptr != 0) {
        uint8_t cap_id = pci_config_read8(pci->bus, pci->slot, pci->func, cap_ptr);
        uint8_t cap_next = pci_config_read8(pci->bus, pci->slot, pci->func, (uint8_t)(cap_ptr + 1));

        if (cap_id == PCI_CAP_ID_VENDOR) {
            uint8_t cfg_type =
                pci_config_read8(pci->bus, pci->slot, pci->func, (uint8_t)(cap_ptr + 3));
            uint8_t bar = pci_config_read8(pci->bus, pci->slot, pci->func, (uint8_t)(cap_ptr + 4));
            uint32_t offset =
                pci_config_read32(pci->bus, pci->slot, pci->func, (uint8_t)(cap_ptr + 8));
            uint32_t length =
                pci_config_read32(pci->bus, pci->slot, pci->func, (uint8_t)(cap_ptr + 12));

            /* BARs (especially 64-bit ones) commonly live way outside */
            /* what Limine's HHDM covers -- QEMU/OVMF placed this one at */
            /* physical ~0xc000000000 in testing, nowhere near the actual */
            /* RAM HHDM maps. Map each capability's region explicitly */
            /* instead of assuming hhdm_offset + phys reaches it. */
            uint64_t base = bar_base(pci, bar);
            volatile uint8_t *region = (volatile uint8_t *)vmm_map_mmio(base + offset, length);

            switch (cfg_type) {
                case VIRTIO_PCI_CAP_COMMON_CFG:
                    vdev->common = (volatile struct virtio_pci_common_cfg *)(uintptr_t)region;
                    break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    vdev->notify_base = region;
                    vdev->notify_off_multiplier =
                        pci_config_read32(pci->bus, pci->slot, pci->func, (uint8_t)(cap_ptr + 16));
                    break;
                case VIRTIO_PCI_CAP_ISR_CFG:
                    vdev->isr = region;
                    break;
                case VIRTIO_PCI_CAP_DEVICE_CFG:
                    vdev->device_cfg = (volatile void *)region;
                    break;
                default:
                    break;
            }
        }

        cap_ptr = (uint8_t)(cap_next & 0xFC);
    }

    if (vdev->common == NULL || vdev->notify_base == NULL) {
        kprintf("virtio: missing required common-cfg or notify-cfg capability\n");
        return -1;
    }

    /* Spec init step 1: reset, then wait for the device to observe it. */
    vdev->common->device_status = 0;
    while (vdev->common->device_status != 0) {
        asm volatile("pause");
    }

    return 0;
}

int virtio_negotiate_features(struct virtio_device *vdev, uint64_t wanted) {
    volatile struct virtio_pci_common_cfg *cfg = vdev->common;

    cfg->device_status = (uint8_t)(cfg->device_status | VIRTIO_STATUS_ACKNOWLEDGE);
    cfg->device_status = (uint8_t)(cfg->device_status | VIRTIO_STATUS_DRIVER);

    cfg->device_feature_select = 0;
    uint64_t device_features = cfg->device_feature;
    cfg->device_feature_select = 1;
    device_features |= ((uint64_t)cfg->device_feature) << 32;

    uint64_t negotiated = (wanted | (1ull << VIRTIO_F_VERSION_1)) & device_features;
    if (!(negotiated & (1ull << VIRTIO_F_VERSION_1))) {
        kprintf("virtio: device does not offer VIRTIO_F_VERSION_1\n");
        return -1;
    }

    cfg->driver_feature_select = 0;
    cfg->driver_feature = (uint32_t)negotiated;
    cfg->driver_feature_select = 1;
    cfg->driver_feature = (uint32_t)(negotiated >> 32);

    cfg->device_status = (uint8_t)(cfg->device_status | VIRTIO_STATUS_FEATURES_OK);
    if (!(cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("virtio: device rejected our feature subset\n");
        cfg->device_status = VIRTIO_STATUS_FAILED;
        return -1;
    }

    return 0;
}

void virtio_driver_ok(struct virtio_device *vdev) {
    vdev->common->device_status = (uint8_t)(vdev->common->device_status | VIRTIO_STATUS_DRIVER_OK);
}

int virtio_queue_init(struct virtio_device *vdev, uint16_t index, struct virtio_queue *q) {
    volatile struct virtio_pci_common_cfg *cfg = vdev->common;

    cfg->queue_select = index;
    uint16_t max_size = cfg->queue_size;
    if (max_size == 0) {
        kprintf("virtio: queue %u not available\n", index);
        return -1;
    }

    /* Each ring part gets its own PMM page, which caps how many */
    /* descriptors fit in the (16-byte-entry) descriptor table -- 256, */
    /* comfortably above what virtio-gpu's control queue needs. */
    uint16_t size = max_size;
    if (size > PMM_PAGE_SIZE / sizeof(struct virtq_desc)) {
        size = PMM_PAGE_SIZE / sizeof(struct virtq_desc);
    }

    uint64_t hhdm_offset = hhdm_request.response->offset;

    uint64_t desc_phys = pmm_alloc_page();
    uint64_t avail_phys = pmm_alloc_page();
    uint64_t used_phys = pmm_alloc_page();
    if (desc_phys == 0 || avail_phys == 0 || used_phys == 0) {
        kprintf("virtio: out of memory setting up queue %u\n", index);
        return -1;
    }

    q->index = index;
    q->size = size;
    q->desc = (volatile struct virtq_desc *)(uintptr_t)(desc_phys + hhdm_offset);
    q->avail = (volatile struct virtq_avail *)(uintptr_t)(avail_phys + hhdm_offset);
    q->used = (volatile struct virtq_used *)(uintptr_t)(used_phys + hhdm_offset);

    memset((void *)(uintptr_t)q->desc, 0, PMM_PAGE_SIZE);
    memset((void *)(uintptr_t)q->avail, 0, PMM_PAGE_SIZE);
    memset((void *)(uintptr_t)q->used, 0, PMM_PAGE_SIZE);

    for (uint16_t i = 0; i < size; i++) {
        q->desc[i].next = (uint16_t)(i + 1);
    }
    q->free_head = 0;
    q->num_free = size;
    q->last_used_idx = 0;

    cfg->queue_size = size;
    cfg->queue_desc = desc_phys;
    cfg->queue_driver = avail_phys;
    cfg->queue_device = used_phys;
    cfg->queue_enable = 1;

    uint16_t notify_off = cfg->queue_notify_off;
    q->notify = (volatile uint16_t *)(vdev->notify_base +
                                      (uint32_t)notify_off * vdev->notify_off_multiplier);

    return 0;
}

static void virtio_queue_free_chain(struct virtio_queue *q, uint16_t head) {
    uint16_t idx = head;
    for (;;) {
        int has_next = q->desc[idx].flags & VIRTQ_DESC_F_NEXT;
        uint16_t next_idx = q->desc[idx].next;
        q->desc[idx].next = q->free_head;
        q->free_head = idx;
        q->num_free++;
        if (!has_next) {
            break;
        }
        idx = next_idx;
    }
}

void virtio_queue_submit_chain(struct virtio_queue *q, const struct virtio_buffer *buffers,
                               int count) {
    if (count <= 0 || count > 8 || q->num_free < (uint16_t)count) {
        kprintf("virtio: submit_chain rejected (count=%d, free=%u)\n", count, q->num_free);
        return;
    }

    uint16_t indices[8];
    uint16_t cur = q->free_head;
    for (int i = 0; i < count; i++) {
        indices[i] = cur;
        cur = q->desc[cur].next; /* Next free slot -- read before we repurpose .next below. */
    }
    q->free_head = cur;
    q->num_free = (uint16_t)(q->num_free - count);

    uint64_t hhdm_offset = hhdm_request.response->offset;
    for (int i = 0; i < count; i++) {
        uint16_t idx = indices[i];
        q->desc[idx].addr = (uint64_t)(uintptr_t)buffers[i].addr - hhdm_offset;
        q->desc[idx].len = buffers[i].len;
        uint16_t flags = buffers[i].device_writable ? VIRTQ_DESC_F_WRITE : 0;
        if (i + 1 < count) {
            flags |= VIRTQ_DESC_F_NEXT;
            q->desc[idx].next = indices[i + 1];
        } else {
            q->desc[idx].next = 0;
        }
        q->desc[idx].flags = flags;
    }

    uint16_t avail_slot = (uint16_t)(q->avail->idx % q->size);
    q->avail->ring[avail_slot] = indices[0];
    asm volatile("" ::: "memory"); /* Ring entry must be visible before idx bumps. */
    q->avail->idx = (uint16_t)(q->avail->idx + 1);
    asm volatile("" ::: "memory");

    *q->notify = q->index;
}

/* Shared by virtio_queue_wait() and virtio_queue_try_wait(): pops one
 * entry off the used ring and reclaims its descriptor chain. Returns 0
 * if the used ring hasn't advanced yet, 1 otherwise. */
static int pop_used(struct virtio_queue *q, uint16_t *out_desc_id, uint32_t *out_len) {
    if (q->used->idx == q->last_used_idx) {
        return 0;
    }

    uint16_t used_slot = (uint16_t)(q->last_used_idx % q->size);
    uint16_t desc_id = (uint16_t)q->used->ring[used_slot].id;
    uint32_t len = q->used->ring[used_slot].len;
    q->last_used_idx++;

    virtio_queue_free_chain(q, desc_id);
    *out_desc_id = desc_id;
    *out_len = len;
    return 1;
}

uint32_t virtio_queue_wait(struct virtio_queue *q) {
    uint16_t desc_id;
    uint32_t len;
    while (!pop_used(q, &desc_id, &len)) {
        asm volatile("pause");
    }
    return len;
}

int virtio_queue_try_wait(struct virtio_queue *q, uint16_t *out_desc_id, uint32_t *out_len) {
    return pop_used(q, out_desc_id, out_len);
}
