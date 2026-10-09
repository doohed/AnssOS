#include "arch/x86_64/fpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "boot/limine.h"
#include "boot/requests.h"
#include "console/fbconsole.h"
#include "console/splash.h"
#include "drivers/display.h"
#include "drivers/pci.h"
#include "drivers/timer.h"
#include "drivers/usb/usb.h"
#include "drivers/usb/usb_kbd.h"
#include "drivers/serial.h"
#include "drivers/virtio/virtio_blk.h"
#include "drivers/virtio/virtio_input.h"
#include "drivers/virtio/virtio_snd.h"
#include "exec/process.h"
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
    fpu_init();
    kprintf("x87/SSE enabled for userland.\n");

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
     * first, hence running this here rather than right after idt_init().
     * timer_init() likewise maps ACPI tables. */
    pic_remap();
    timer_init();
    asm volatile("sti");
    timer_check(); /* Never trust that ticks arrive -- a real PC may not route them. */
    timer_log_status();
    kprintf("Interrupts enabled.\n");
    kprintf("M2 complete.\n");

    pci_enumerate();
    kprintf("M3 complete.\n");

    /* virtio-gpu under QEMU, the firmware's GOP framebuffer on a real PC
     * (see drivers/display.c). */
    struct framebuffer fb;
    if (display_init(&fb) == 0) {
        kprintf("Display: %s, %ux%u.\n", display_name(), fb.width, fb.height);

        /* fbconsole_init() must run first -- it's what sets fbconsole.c's
         * internal framebuffer pointer, which splash_show() relies on via
         * fbconsole_draw_text_at(). It also clears the screen to black,
         * which doubles as the splash's blank backdrop. */
        fbconsole_init(&fb);
        splash_show(&fb);  /* Logo + looping "..." while the rest of boot logs to serial. */
        fbconsole_clear(); /* Wipe the splash before the scrolling log console takes over. */

        kprintf_set_sink(fbconsole_kprintf_sink);
        fbconsole_write("AnssOS -- x86_64 / UEFI / Limine / ");
        fbconsole_write(display_name());
        fbconsole_write("\n\n");
        kprintf("M5 complete: framebuffer console live via %s.\n", display_name());
        timer_log_status(); /* Again, now that it reaches the screen too. */
    } else {
        kprintf("Skipping M5 (no display: no virtio-gpu, no firmware framebuffer).\n");
    }

    /* The keyboard is optional: COM1 always works as one (see
     * drivers/input.c), and a real PC with no keyboard driver yet should
     * still reach the shell, if only to show that everything else did. */
    int virtio_kbd = virtio_input_init() == 0;
    if (virtio_kbd) {
        kprintf("M6 complete: virtio-input keyboard ready.\n");
    }
    int xhci_count = usb_init();
    if (usb_kbd_count() > 0) {
        kprintf("USB keyboard ready (%d xHCI controller(s)).\n", xhci_count);
    } else if (!virtio_kbd) {
        kprintf("No keyboard found (%d xHCI controller(s)) -- one plugged in later still works; "
                "until then, input only over COM1, if there is one.\n",
                xhci_count);
    }

    vfs_init();
    if (vfs_root() == NULL) {
        kprintf("PANIC: no memory for the filesystem root -- see the PMM line above\n");
        hcf();
    }
    kprintf("M7 complete: in-memory filesystem ready.\n");

    if (virtio_blk_init() == 0) {
        blkfs_load(); /* No-op (not an error) on a blank/unformatted disk. */
        kprintf("M9 complete: persistent storage ready.\n");
    } else {
        kprintf(
            "Skipping M9 (no virtio-blk device) -- filesystem stays in-memory only. "
            "Boot QEMU with -device virtio-blk-pci for persistence.\n");
    }

    if (virtio_snd_init() == 0) {
        kprintf("M17 complete: virtio-sound ready.\n");
    } else {
        kprintf(
            "Skipping M17 (no virtio-sound device) -- audio playback unavailable. Boot "
            "QEMU with -device virtio-sound-pci for `play`.\n");
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
    /* Programs live in /bin, which the shell searches for bare
     * command names (see shell.c's resolve_program()) -- so `scarf`
     * works from any directory, not just the one holding it. */
    vfs_mkdir(vfs_root(), "bin");
    struct vnode *bin = vfs_resolve(vfs_root(), "/bin");
    if (bin == NULL) {
        bin = vfs_root(); /* mkdir failed -- fall back to the old layout. */
    }

    vfs_write_bytes(bin, "hello", hello_elf_start, (size_t)(hello_elf_end - hello_elf_start));
    vfs_write_bytes(bin, "crash", crash_elf_start, (size_t)(crash_elf_end - crash_elf_start));
    vfs_write_bytes(bin, "malloctest", malloctest_elf_start,
                    (size_t)(malloctest_elf_end - malloctest_elf_start));
    vfs_write_bytes(bin, "filetest", filetest_elf_start,
                    (size_t)(filetest_elf_end - filetest_elf_start));
    vfs_write_file(vfs_root(), "filetest.txt", "hello file test\n", 0);
    vfs_write_bytes(bin, "dirtest", dirtest_elf_start,
                    (size_t)(dirtest_elf_end - dirtest_elf_start));
    vfs_write_bytes(bin, "forktest", forktest_elf_start,
                    (size_t)(forktest_elf_end - forktest_elf_start));
    vfs_write_bytes(bin, "forkchild", forkchild_elf_start,
                    (size_t)(forkchild_elf_end - forkchild_elf_start));
    vfs_write_bytes(bin, "preempttest", preempttest_elf_start,
                    (size_t)(preempttest_elf_end - preempttest_elf_start));
    vfs_write_bytes(bin, "termtest", termtest_elf_start,
                    (size_t)(termtest_elf_end - termtest_elf_start));
    vfs_write_bytes(bin, "readdirtest", readdirtest_elf_start,
                    (size_t)(readdirtest_elf_end - readdirtest_elf_start));
    vfs_write_bytes(bin, "pipetest", pipetest_elf_start,
                    (size_t)(pipetest_elf_end - pipetest_elf_start));
    /* Not a self-test fixture -- sh.bin is a real tool (`run sh`, or
     * spawned as a tile.c pane), embedded the same way scarf/play
     * are. */
    vfs_write_bytes(bin, "sh", sh_elf_start, (size_t)(sh_elf_end - sh_elf_start));
    vfs_write_bytes(bin, "tile", tile_elf_start, (size_t)(tile_elf_end - tile_elf_start));
    /* Not a self-test fixture like the rest -- scarf.bin is an actual
     * tool (`run scarf.bin`), embedded the same way for the same
     * reason: there's no host-side way to get a file onto the VFS. */
    vfs_write_bytes(bin, "scarf", scarf_elf_start, (size_t)(scarf_elf_end - scarf_elf_start));
    /* Likewise play.bin -- plus testtone.wav, a synthesized fixture
     * (scripts/gen-test-tone.py) so `play testtone.wav` works out of
     * the box with no host-side file provisioning step. */
    vfs_write_bytes(bin, "play", play_elf_start, (size_t)(play_elf_end - play_elf_start));
    vfs_write_bytes(vfs_root(), "testtone.wav", testtone_wav_start,
                    (size_t)(testtone_wav_end - testtone_wav_start));

    /* Boot into the userland shell, /bin/sh (userland/rust/sh/). If
     * it ever exits -- `exit`, or a crash -- fall back to the
     * kernel-resident shell below, which also has the kernel
     * diagnostics (meminfo, lspci, crash, reboot, ...) sh can't
     * reach; `sh` there starts it again. */
    struct vnode *sh = vfs_resolve(vfs_root(), "/bin/sh");
    if (sh != NULL && sh->type == VNODE_FILE) {
        const char *const sh_argv[] = {"sh"};
        if (process_spawn(sh->data, sh->size, 1, sh_argv, vfs_root(), KERNEL_PARENT_PID) >= 0) {
            scheduler_run_until(-1);
        }
        kprintf("\x1b[0m\nsh exited -- this is the kernel shell (`sh` starts it again)\n");
    }

    /* The deliberate #DE self-test that used to always run here
     * (proving the M1 exception handler works) is now the shell's
     * `crash` builtin -- trigger it on demand instead of
     * automatically, since the handler halts forever and we want an
     * interactive prompt instead. shell_run() never returns. */
    shell_run();

    hcf();
}
