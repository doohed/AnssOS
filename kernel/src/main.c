#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "boot/limine.h"
#include "boot/requests.h"
#include "console/fbconsole.h"
#include "console/splash.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "drivers/serial.h"
#include "drivers/virtio/virtio_blk.h"
#include "drivers/virtio/virtio_gpu.h"
#include "drivers/virtio/virtio_input.h"
#include "exec/userland_blobs.h"
#include "fs/blkfs.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "shell/shell.h"

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

    heap_init();
    kprintf("Heap ready (kmalloc/kfree over the PMM).\n");

    /* VMM smoke test (M10 prep): create a fresh address space, map a
     * scratch page into it as user-accessible, switch into it, write/
     * read through that mapping, then switch back to the boot address
     * space -- proves the per-address-space page-table plumbing works
     * before anything ever runs in ring 3. */
    {
        struct addr_space boot_as = vmm_current_address_space();
        struct addr_space test_as = vmm_new_address_space();

        uint64_t scratch_phys = pmm_alloc_page();
        uint64_t scratch_virt = 0x400000;
        vmm_map(&test_as, scratch_virt, scratch_phys, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);

        vmm_switch(&test_as);
        volatile uint64_t *p = (volatile uint64_t *)scratch_virt;
        *p = 0xDEADBEEFCAFEBABEull;
        uint64_t readback = *p;
        vmm_switch(&boot_as);

        pmm_free_page(scratch_phys);

        if (readback != 0xDEADBEEFCAFEBABEull) {
            kprintf(
                "VMM PANIC: address-space smoke test read back 0x%lx, expected something else\n",
                readback);
            hcf();
        }
        kprintf("VMM self-test OK: per-address-space page tables work\n");
    }

    /* pic_remap() maps the Local APIC's MMIO page (vmm_map_mmio(), see
     * arch/x86_64/pic.c) to relay the legacy PIC's interrupts through
     * it -- needs the PMM (and its own page-table allocations) up
     * first, hence running this here rather than right after idt_init(). */
    pic_remap();
    pit_init();
    asm volatile("sti");
    kprintf("Interrupts enabled (PIT @ %u Hz).\n", PIT_HZ);
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

        /* fbconsole_init() must run first -- it's what sets fbconsole.c's
         * internal framebuffer pointer, which splash_show() relies on via
         * fbconsole_draw_text_at(). It also clears the screen to black,
         * which doubles as the splash's blank backdrop. */
        fbconsole_init(&fb);
        splash_show(&fb);  /* Logo + looping "..." while the rest of boot logs to serial. */
        fbconsole_clear(); /* Wipe the splash before the scrolling log console takes over. */

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

    if (virtio_input_init() == 0) {
        kprintf("M6 complete: virtio-input keyboard ready.\n");

        vfs_init();
        kprintf("M7 complete: in-memory filesystem ready.\n");

        if (virtio_blk_init() == 0) {
            blkfs_load(); /* No-op (not an error) on a blank/unformatted disk. */
            kprintf("M9 complete: persistent storage ready.\n");
        } else {
            kprintf(
                "Skipping M9 (no virtio-blk device) -- filesystem stays in-memory only. "
                "Boot QEMU with -device virtio-blk-pci for persistence.\n");
        }

        /* M10/M11 self-test fixtures: the hand-rolled userland test
         * payloads (see userland/, embedded into the kernel image via
         * exec/userland_blobs.S) get written fresh onto the in-memory VFS
         * on every boot, so `run <name>.bin` always has something to load
         * without any host-side provisioning step -- same idea as the
         * PMM/VMM self-tests above, just landing on the filesystem
         * instead of just printing a result. Not persisted to disk;
         * there's nothing to save here. filetest.txt is filetest.bin's
         * own fixture -- a known file for it to open/read/write/lseek
         * against. dirtest.bin needs no fixture -- it creates its own
         * directory and file via mkdir()/O_CREAT. */
        vfs_write_bytes(vfs_root(), "hello.bin", hello_elf_start,
                        (size_t)(hello_elf_end - hello_elf_start));
        vfs_write_bytes(vfs_root(), "crash.bin", crash_elf_start,
                        (size_t)(crash_elf_end - crash_elf_start));
        vfs_write_bytes(vfs_root(), "malloctest.bin", malloctest_elf_start,
                        (size_t)(malloctest_elf_end - malloctest_elf_start));
        vfs_write_bytes(vfs_root(), "filetest.bin", filetest_elf_start,
                        (size_t)(filetest_elf_end - filetest_elf_start));
        vfs_write_file(vfs_root(), "filetest.txt", "hello file test\n", 0);
        vfs_write_bytes(vfs_root(), "dirtest.bin", dirtest_elf_start,
                        (size_t)(dirtest_elf_end - dirtest_elf_start));
        vfs_write_bytes(vfs_root(), "forktest.bin", forktest_elf_start,
                        (size_t)(forktest_elf_end - forktest_elf_start));
        vfs_write_bytes(vfs_root(), "forkchild.bin", forkchild_elf_start,
                        (size_t)(forkchild_elf_end - forkchild_elf_start));
        vfs_write_bytes(vfs_root(), "preempttest.bin", preempttest_elf_start,
                        (size_t)(preempttest_elf_end - preempttest_elf_start));
        vfs_write_bytes(vfs_root(), "termtest.bin", termtest_elf_start,
                        (size_t)(termtest_elf_end - termtest_elf_start));
        vfs_write_bytes(vfs_root(), "readdirtest.bin", readdirtest_elf_start,
                        (size_t)(readdirtest_elf_end - readdirtest_elf_start));
        /* Not a self-test fixture like the rest -- scarf.bin is an actual
         * tool (`run scarf.bin`), embedded the same way for the same
         * reason: there's no host-side way to get a file onto the VFS. */
        vfs_write_bytes(vfs_root(), "scarf.bin", scarf_elf_start,
                        (size_t)(scarf_elf_end - scarf_elf_start));

        /* The deliberate #DE self-test that used to always run here
         * (proving the M1 exception handler works) is now the shell's
         * `crash` builtin -- trigger it on demand instead of
         * automatically, since the handler halts forever and we want an
         * interactive prompt instead. shell_run() never returns. */
        shell_run();
    } else {
        kprintf(
            "Skipping M6 and the shell (no virtio-input keyboard) -- boot QEMU with "
            "-device virtio-keyboard-pci\n");
    }

    hcf();
}
