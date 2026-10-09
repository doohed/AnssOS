#include "display.h"
#include "pci.h"
#include "serial.h"
#include "virtio/virtio_gpu.h"
#include "../boot/requests.h"
#include "../lib/string.h"
#include "../mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

static enum { DISPLAY_NONE, DISPLAY_VIRTIO, DISPLAY_GOP } kind;
static struct framebuffer screen; /* What callers draw into. */

/* GOP only: the real framebuffer, which `screen` is a RAM copy of. Limine
 * maps it into the HHDM as write-combining, so plain stores to it are
 * fast; reads are not, and nothing here does any. */
static volatile uint8_t *gop_base;
static uint64_t gop_pitch_bytes;

static int gop_init(void) {
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count == 0) {
        kprintf("display: no firmware framebuffer from the bootloader\n");
        return -1;
    }
    struct limine_framebuffer *lfb = framebuffer_request.response->framebuffers[0];

    /* BGRX8888 is what everything above this draws, and what UEFI GOP
     * framebuffers are in practice; anything else would need converting
     * on every flush, so it's refused instead. */
    if (lfb->bpp != 32 || lfb->memory_model != LIMINE_FRAMEBUFFER_RGB ||
        lfb->red_mask_shift != 16 || lfb->green_mask_shift != 8 || lfb->blue_mask_shift != 0) {
        kprintf("display: unsupported framebuffer format (%u bpp, R@%u G@%u B@%u)\n", lfb->bpp,
                lfb->red_mask_shift, lfb->green_mask_shift, lfb->blue_mask_shift);
        return -1;
    }

    uint64_t shadow_bytes = lfb->width * lfb->height * sizeof(uint32_t);
    uint64_t pages = (shadow_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t shadow_phys = pmm_alloc_pages(pages);
    if (shadow_phys == 0) {
        kprintf("display: no memory for a %lu KiB framebuffer copy\n", shadow_bytes / 1024);
        return -1;
    }

    screen.pixels = (volatile uint32_t *)(uintptr_t)(shadow_phys + hhdm_request.response->offset);
    screen.width = (uint32_t)lfb->width;
    screen.height = (uint32_t)lfb->height;
    screen.pitch = (uint32_t)lfb->width;
    memset((void *)(uintptr_t)screen.pixels, 0, shadow_bytes);

    gop_base = (volatile uint8_t *)lfb->address;
    gop_pitch_bytes = lfb->pitch;
    kprintf("display: GOP framebuffer %ux%u, pitch %lu bytes\n", screen.width, screen.height,
            gop_pitch_bytes);
    return 0;
}

int display_init(struct framebuffer *out) {
    if (pci_find_device(VIRTIO_GPU_PCI_VENDOR_ID, VIRTIO_GPU_PCI_DEVICE_ID) != NULL &&
        virtio_gpu_init(&screen) == 0) {
        kind = DISPLAY_VIRTIO;
    } else if (gop_init() == 0) {
        kind = DISPLAY_GOP;
    } else {
        return -1;
    }
    *out = screen;
    return 0;
}

const char *display_name(void) {
    switch (kind) {
        case DISPLAY_VIRTIO:
            return "virtio-gpu";
        case DISPLAY_GOP:
            return "GOP framebuffer";
        default:
            return NULL;
    }
}

void display_flush(void) {
    display_flush_rect(0, 0, screen.width, screen.height);
}

void display_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (kind == DISPLAY_VIRTIO) {
        virtio_gpu_flush(); /* Always the whole screen; cheap for QEMU. */
        return;
    }
    if (kind != DISPLAY_GOP || x >= screen.width || y >= screen.height) {
        return;
    }
    if (w > screen.width - x) {
        w = screen.width - x;
    }
    if (h > screen.height - y) {
        h = screen.height - y;
    }

    /* One `rep movsb` per row: fast-string moves are the quickest way to
     * stream into write-combining memory without SSE, which the kernel
     * isn't allowed to touch (see kernel/GNUmakefile). */
    for (uint32_t row = y; row < y + h; row++) {
        const void *src =
            (const void *)(uintptr_t)(screen.pixels + (uint64_t)row * screen.pitch + x);
        void *dst = (void *)(uintptr_t)(gop_base + row * gop_pitch_bytes + x * sizeof(uint32_t));
        uint64_t n = (uint64_t)w * sizeof(uint32_t);
        asm volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    }
}
