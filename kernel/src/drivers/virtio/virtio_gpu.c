#include "virtio_gpu.h"
#include "virtio.h"
#include "../pci.h"
#include "../serial.h"
#include "../../boot/requests.h"
#include "../../lib/string.h"
#include "../../mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

#define VIRTIO_GPU_RESP_OK_NODATA 0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_MAX_SCANOUTS 16

#define GPU_RESOURCE_ID 1

#define FB_DEFAULT_WIDTH 1024
#define FB_DEFAULT_HEIGHT 768
#define FB_MAX_WIDTH 1920
#define FB_MAX_HEIGHT 1080

struct __attribute__((packed)) virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
};

struct __attribute__((packed)) virtio_gpu_rect {
    uint32_t x, y, width, height;
};

struct __attribute__((packed)) virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
};

struct __attribute__((packed)) virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct __attribute__((packed)) virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct __attribute__((packed)) virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct __attribute__((packed)) virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct virtio_gpu_mem_entry entries[1];
};

struct __attribute__((packed)) virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct __attribute__((packed)) virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct __attribute__((packed)) virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
};

static struct virtio_device vdev;
static struct virtio_queue controlq;
static struct virtio_gpu_fb fb;

/* Scratch request/response buffers for GPU commands. These have to be */
/* PMM-backed (HHDM) memory, not stack/.bss, since virtqueue descriptors */
/* carry physical addresses -- see the comment on struct virtio_buffer. */
static void *cmd_req_virt;
static void *cmd_resp_virt;

static int cmd_scratch_init(void) {
    uint64_t req_phys = pmm_alloc_page();
    uint64_t resp_phys = pmm_alloc_page();
    if (req_phys == 0 || resp_phys == 0) {
        return -1;
    }
    uint64_t hhdm_offset = hhdm_request.response->offset;
    cmd_req_virt = (void *)(uintptr_t)(req_phys + hhdm_offset);
    cmd_resp_virt = (void *)(uintptr_t)(resp_phys + hhdm_offset);
    return 0;
}

/* Every virtio-gpu control command follows the same request/response */
/* shape: copy into the scratch buffers, submit as a 2-descriptor chain */
/* (request readable, response writable), and poll for completion. */
static void gpu_cmd(const void *req, uint32_t req_len, void *resp, uint32_t resp_len) {
    memcpy(cmd_req_virt, req, req_len);
    memset(cmd_resp_virt, 0, resp_len);

    struct virtio_buffer bufs[2] = {
        {.addr = cmd_req_virt, .len = req_len, .device_writable = 0},
        {.addr = cmd_resp_virt, .len = resp_len, .device_writable = 1},
    };
    virtio_queue_submit_chain(&controlq, bufs, 2);
    virtio_queue_wait(&controlq);

    memcpy(resp, cmd_resp_virt, resp_len);
}

static int get_display_info(uint32_t *out_width, uint32_t *out_height) {
    struct virtio_gpu_ctrl_hdr req;
    struct virtio_gpu_resp_display_info resp;
    memset(&req, 0, sizeof(req));
    req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    gpu_cmd(&req, sizeof(req), &resp, sizeof(resp));

    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        kprintf("virtio-gpu: GET_DISPLAY_INFO failed (resp type 0x%x)\n", resp.hdr.type);
        return -1;
    }
    if (!resp.pmodes[0].enabled || resp.pmodes[0].r.width == 0 || resp.pmodes[0].r.height == 0) {
        return -1;
    }

    uint32_t width = resp.pmodes[0].r.width;
    uint32_t height = resp.pmodes[0].r.height;
    *out_width = width > FB_MAX_WIDTH ? FB_MAX_WIDTH : width;
    *out_height = height > FB_MAX_HEIGHT ? FB_MAX_HEIGHT : height;
    return 0;
}

int virtio_gpu_init(struct virtio_gpu_fb *out_fb) {
    const struct pci_device *pci =
        pci_find_device(VIRTIO_GPU_PCI_VENDOR_ID, VIRTIO_GPU_PCI_DEVICE_ID);
    if (pci == NULL) {
        kprintf("virtio-gpu: device not found\n");
        return -1;
    }

    if (virtio_pci_init(pci, &vdev) != 0 || virtio_negotiate_features(&vdev, 0) != 0 ||
        virtio_queue_init(&vdev, 0, &controlq) != 0) {
        return -1;
    }
    virtio_driver_ok(&vdev);
    kprintf("virtio-gpu: init handshake complete (ACKNOWLEDGE/DRIVER/FEATURES_OK/DRIVER_OK)\n");

    if (cmd_scratch_init() != 0) {
        kprintf("virtio-gpu: out of memory allocating command scratch buffers\n");
        return -1;
    }

    uint32_t width = FB_DEFAULT_WIDTH;
    uint32_t height = FB_DEFAULT_HEIGHT;
    if (get_display_info(&width, &height) == 0) {
        kprintf("virtio-gpu: display reports %ux%u\n", width, height);
    } else {
        kprintf("virtio-gpu: no usable display info, defaulting to %ux%u\n", width, height);
    }

    struct virtio_gpu_resource_create_2d create_req;
    struct virtio_gpu_ctrl_hdr create_resp;
    memset(&create_req, 0, sizeof(create_req));
    create_req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create_req.resource_id = GPU_RESOURCE_ID;
    create_req.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create_req.width = width;
    create_req.height = height;
    gpu_cmd(&create_req, sizeof(create_req), &create_resp, sizeof(create_resp));
    if (create_resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("virtio-gpu: RESOURCE_CREATE_2D failed (0x%x)\n", create_resp.type);
        return -1;
    }

    uint64_t fb_bytes = (uint64_t)width * height * 4;
    uint64_t fb_pages = (fb_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t fb_phys = pmm_alloc_pages(fb_pages);
    if (fb_phys == 0) {
        kprintf("virtio-gpu: out of memory allocating %lu-page framebuffer\n", fb_pages);
        return -1;
    }
    uint64_t hhdm_offset = hhdm_request.response->offset;
    fb.pixels = (volatile uint32_t *)(uintptr_t)(fb_phys + hhdm_offset);
    fb.width = width;
    fb.height = height;
    memset((void *)(uintptr_t)fb.pixels, 0, fb_pages * PMM_PAGE_SIZE);

    struct virtio_gpu_resource_attach_backing attach_req;
    struct virtio_gpu_ctrl_hdr attach_resp;
    memset(&attach_req, 0, sizeof(attach_req));
    attach_req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach_req.resource_id = GPU_RESOURCE_ID;
    attach_req.nr_entries = 1;
    attach_req.entries[0].addr = fb_phys;
    attach_req.entries[0].length = (uint32_t)(fb_pages * PMM_PAGE_SIZE);
    gpu_cmd(&attach_req, sizeof(attach_req), &attach_resp, sizeof(attach_resp));
    if (attach_resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("virtio-gpu: RESOURCE_ATTACH_BACKING failed (0x%x)\n", attach_resp.type);
        return -1;
    }

    struct virtio_gpu_set_scanout scanout_req;
    struct virtio_gpu_ctrl_hdr scanout_resp;
    memset(&scanout_req, 0, sizeof(scanout_req));
    scanout_req.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout_req.r.width = width;
    scanout_req.r.height = height;
    scanout_req.scanout_id = 0;
    scanout_req.resource_id = GPU_RESOURCE_ID;
    gpu_cmd(&scanout_req, sizeof(scanout_req), &scanout_resp, sizeof(scanout_resp));
    if (scanout_resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("virtio-gpu: SET_SCANOUT failed (0x%x)\n", scanout_resp.type);
        return -1;
    }

    kprintf("virtio-gpu: resource %u (%ux%u) created, backed, and set as scanout 0\n",
            GPU_RESOURCE_ID, width, height);

    *out_fb = fb;
    return 0;
}

void virtio_gpu_flush(void) {
    struct virtio_gpu_transfer_to_host_2d xfer_req;
    struct virtio_gpu_ctrl_hdr xfer_resp;
    memset(&xfer_req, 0, sizeof(xfer_req));
    xfer_req.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer_req.r.width = fb.width;
    xfer_req.r.height = fb.height;
    xfer_req.resource_id = GPU_RESOURCE_ID;
    gpu_cmd(&xfer_req, sizeof(xfer_req), &xfer_resp, sizeof(xfer_resp));

    struct virtio_gpu_resource_flush flush_req;
    struct virtio_gpu_ctrl_hdr flush_resp;
    memset(&flush_req, 0, sizeof(flush_req));
    flush_req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush_req.r.width = fb.width;
    flush_req.r.height = fb.height;
    flush_req.resource_id = GPU_RESOURCE_ID;
    gpu_cmd(&flush_req, sizeof(flush_req), &flush_resp, sizeof(flush_resp));
}
