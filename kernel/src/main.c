#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "boot/limine.h"
#include "boot/requests.h"
#include "console/fbconsole.h"
#include "drivers/pci.h"
#include "drivers/serial.h"
#include "drivers/virtio/virtio_gpu.h"
#include "mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

static void hcf(void) {
    for (;;) {
        asm volatile("cli; hlt");
    }
}

void kmain(void) {
    serial_init();
    kprintf("\nAnssOS booting (x86_64 / UEFI / Limine)\n");

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        kprintf("PANIC: bootloader does not support the requested base revision\n");
        hcf();
    }

    if (hhdm_request.response != NULL) {
        kprintf("HHDM offset: 0x%lx\n", hhdm_request.response->offset);
    }

    if (memmap_request.response != NULL) {
        struct limine_memmap_response *mm = memmap_request.response;
        uint64_t usable_bytes = 0;
        for (uint64_t i = 0; i < mm->entry_count; i++) {
            struct limine_memmap_entry *e = mm->entries[i];
            if (e->type == LIMINE_MEMMAP_USABLE) {
                usable_bytes += e->length;
            }
        }
        kprintf("Memory map: %lu entries, %lu KiB usable\n", mm->entry_count, usable_bytes / 1024);
    }

    if (framebuffer_request.response != NULL &&
        framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
        kprintf("Boot framebuffer: %lux%lu @ %u bpp, pitch %lu\n", fb->width, fb->height, fb->bpp,
                fb->pitch);
    }

    if (rsdp_request.response != NULL) {
        kprintf("RSDP: %p\n", rsdp_request.response->address);
    }

    kprintf("M0 complete.\n");

    gdt_init();
    kprintf("GDT/TSS loaded.\n");
    idt_init();
    kprintf("IDT loaded.\n");

    pmm_init();

    /* Smoke-test the allocator: grab a batch of pages, confirm they're */
    /* all distinct, then give them back and confirm the free count */
    /* returns to where it started. */
    uint64_t free_before = pmm_free_page_count();
    uint64_t pages[16];
    for (int i = 0; i < 16; i++) {
        pages[i] = pmm_alloc_page();
        for (int j = 0; j < i; j++) {
            if (pages[i] != 0 && pages[i] == pages[j]) {
                kprintf("PMM PANIC: pmm_alloc_page returned a duplicate address\n");
                hcf();
            }
        }
    }
    for (int i = 0; i < 16; i++) {
        pmm_free_page(pages[i]);
    }
    if (pmm_free_page_count() != free_before) {
        kprintf("PMM PANIC: free page count did not return to baseline after freeing\n");
        hcf();
    }
    kprintf("PMM self-test OK: allocated/freed 16 distinct pages\n");
    kprintf("M2 complete.\n");

    pci_enumerate();
    const struct pci_device *gpu = pci_find_device(0x1af4, 0x1050);
    if (gpu != NULL) {
        kprintf("Found virtio-gpu-pci at %u:%u.%u (BAR0=0x%x)\n", gpu->bus, gpu->slot, gpu->func,
                gpu->bar[0]);
    } else {
        kprintf("virtio-gpu-pci not found -- boot QEMU with -device virtio-gpu-pci\n");
    }
    kprintf("M3 complete.\n");

    struct virtio_gpu_fb fb;
    if (gpu != NULL && virtio_gpu_init(&fb) == 0) {
        kprintf("M4 complete.\n");

        fbconsole_init(
            &fb); /* Clears the framebuffer to black -- do this before drawing anything. */
        kprintf_set_sink(fbconsole_kprintf_sink);
        fbconsole_write("AnssOS -- x86_64 / UEFI / Limine / virtio-gpu\n\n");
        kprintf("M5 complete: framebuffer console live via virtio-gpu.\n");

        /* Paint a small gradient swatch in the bottom-right corner --
         * proof of direct pixel writes through the driver (not just text),
         * and that this is virtio-gpu rendering, not the bootloader's boot
         * framebuffer, since we never touch Limine's framebuffer_request
         * pointer here. Kept clear of the console's text region above. */
        uint32_t swatch_w = fb.width / 6;
        uint32_t swatch_h = fb.height / 6;
        uint32_t ox = fb.width - swatch_w;
        uint32_t oy = fb.height - swatch_h;
        for (uint32_t y = 0; y < swatch_h; y++) {
            for (uint32_t x = 0; x < swatch_w; x++) {
                uint32_t r = (x * 255) / swatch_w;
                uint32_t g = (y * 255) / swatch_h;
                uint32_t b = 255 - r;
                fb.pixels[(oy + y) * fb.width + (ox + x)] = (b << 16) | (g << 8) | r; /* BGRX8888 */
            }
        }
        virtio_gpu_flush();
    } else {
        kprintf("Skipping M4/M5 (no virtio-gpu-pci device).\n");
    }

    kprintf("Triggering a deliberate #DE to verify exception handling...\n");

    /* Forces a real DIV instruction with a zero divisor -- inline asm so */
    /* the compiler can't optimize away or reorder around the fault the */
    /* way it could with `1 / (volatile int)0` at -O2. Vector 0, #DE, no */
    /* error code. Never returns: isr_handler halts after dumping state. */
    asm volatile(
        "xor %%edx, %%edx\n"
        "mov $1, %%eax\n"
        "xor %%ecx, %%ecx\n"
        "div %%ecx\n"
        :
        :
        : "eax", "ecx", "edx");

    kprintf("unreachable if the #DE handler fired correctly.\n");
    hcf();
}
