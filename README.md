# AnssOS

A hobby x86_64 operating system, booted via UEFI through
[Limine](https://github.com/limine-bootloader/limine), with a from-scratch
kernel that talks to hardware exclusively through **virtio** devices (QEMU
is the target platform). This is a rewrite of an earlier 2024 legacy-BIOS/
32-bit prototype — that version is still in git history, but the tree now
targets UEFI + long mode only.

## Architecture

- **Bootloader:** Limine (vendored as a git submodule pinned to the
  `v11.x-binary` release branch), booting the kernel over UEFI only — no
  legacy BIOS path.
- **Kernel:** a 64-bit ELF, linked to the top of the address space
  (`0xffffffff80000000`) per the Limine boot protocol, loaded directly by
  Limine (no separate `kernel_entry` stub needed — `kmain` is the ELF entry
  point).
- **Memory:** ordinary RAM access rides on Limine's higher-half direct map
  (HHDM) — `virtual = physical + hhdm_offset`. PCI BAR MMIO does **not**
  live in the HHDM (Limine only maps memory the memory map describes;
  QEMU/OVMF routinely place 64-bit BARs far above actual RAM — virtio-gpu's
  landed at physical `0xc000000000` in testing here, way past a 256 MiB
  guest's real memory), so there's a minimal page-table mapper
  (`mm/vmm.c`) just for that: it walks/extends the page tables Limine
  already built, allocating any missing levels via the PMM, to map a given
  physical MMIO range into a dedicated slice of virtual address space. No
  unmapping, no per-process address spaces, no demand paging — those stay
  future work.
- **Drivers:** virtio-only. The kernel enumerates PCI itself and speaks the
  virtio 1.x ("modern") PCI transport directly — no legacy virtio, no
  non-virtio device drivers.
- **Debugging:** all kernel logging goes out over the COM1 serial port
  (`kprintf`), independent of the display, so anything after boot is
  debuggable via `-serial stdio` even before the framebuffer driver works.

## Repo layout

```
AnssOS/
├── limine/              # git submodule: prebuilt Limine UEFI bootloader
├── limine.conf           # Limine boot menu, points at the kernel
├── .clang-format          # code style, enforced by scripts/format.sh
├── GNUmakefile            # thin wrapper around scripts/*.sh (see below)
├── scripts/
│   ├── build-iso.sh        # builds kernel/bin/kernel + assembles AnssOS.iso
│   ├── run-qemu.sh          # boots an already-built AnssOS.iso in QEMU
│   └── format.sh             # clang-format wrapper (apply or --check)
└── kernel/
    ├── linker.ld           # higher-half link script (Limine protocol layout)
    ├── GNUmakefile
    └── src/
        ├── main.c                    # kmain: init order, banner
        ├── boot/limine.h             # vendored Limine protocol header
        ├── boot/requests.{c,h}       # Limine boot requests (hhdm, memmap, ...)
        ├── arch/x86_64/io.h          # port I/O (in/out b/w/l)
        ├── arch/x86_64/gdt.{c,h}     # 64-bit GDT + TSS
        ├── arch/x86_64/idt.{c,h}     # IDT + exception dispatch (isr.S has the stubs)
        ├── mm/pmm.{c,h}              # bitmap physical page allocator
        ├── mm/vmm.{c,h}              # minimal page-table mapper, MMIO only
        ├── drivers/serial.{c,h}      # COM1 driver + kprintf
        ├── drivers/pci.{c,h}         # legacy config-space PCI enumeration
        ├── drivers/virtio/virtio.{c,h}      # virtio-pci transport + virtqueues
        ├── drivers/virtio/virtio_gpu.{c,h}  # virtio-gpu 2D driver
        ├── console/fbconsole.{c,h}   # bitmap-font text console over the gpu framebuffer
        ├── console/font8x8_basic.h   # vendored public-domain 8x8 font (dhepper/font8x8)
        └── lib/string.{c,h}          # freestanding mem*/str* functions
```

## Building and running

Needs `nasm`, `qemu-system-x86_64`, OVMF firmware, `xorriso`, and `mtools`:

```sh
sudo apt-get install -y nasm qemu-system-x86 ovmf xorriso mtools
```

Building and running are two separate scripts (`make`/`make run` are thin
wrappers around them, if you'd rather not call them directly):

```sh
./scripts/build-iso.sh   # build kernel/bin/kernel and assemble AnssOS.iso
./scripts/run-qemu.sh    # boot the already-built ISO (UEFI/OVMF, virtio-gpu-pci, serial on stdout)
```

`run-qemu.sh` does **not** build anything itself — it errors out if
`AnssOS.iso` doesn't exist yet, telling you to run `build-iso.sh` first.
It auto-detects the local OVMF firmware layout (combined `OVMF.fd` or split
`OVMF_CODE`/`OVMF_VARS`).

### Code style

`.clang-format` at the repo root defines the style (4-space indent, braces
attached, pointers glued to the name). `scripts/format.sh` applies it to
every `kernel/src/**/*.{c,h}` file except the two vendored headers
(`boot/limine.h`, `console/font8x8_basic.h`), which stay as fetched
upstream:

```sh
sudo apt-get install -y clang-format
./scripts/format.sh            # reformat in place
./scripts/format.sh --check    # verify only, non-zero exit if anything's off (CI-friendly)
```

## Roadmap

Built as a sequence of milestones, all now **boot-verified**: booted in
QEMU (`-M q35 -vga none -device virtio-gpu-pci`, OVMF) with a screendump
confirming the console text and a pixel swatch actually render through the
virtio-gpu queue.

- [x] **M0 — Boot to serial.** Limine hands off to `kmain` in 64-bit long
      mode; kernel reads the HHDM offset, memory map, boot framebuffer info,
      and RSDP pointer from Limine and logs them over COM1.
- [x] **M1 — GDT/TSS/IDT.** Own 64-bit GDT and TSS, full IDT with exception
      handlers that dump CPU state over serial (and, once up, the
      framebuffer console) instead of triple-faulting silently. `kmain`
      deliberately triggers a `#DE` (divide-by-zero) after setup to prove
      the dispatch path end-to-end.
- [x] **M2 — Physical memory manager.** Bitmap page allocator built from
      Limine's memory map, plus a contiguous-run allocator
      (`pmm_alloc_pages`) for DMA buffers like the GPU framebuffer. Self-
      tests itself (alloc/free a batch, check for dupes and for the free
      count returning to baseline) on every boot.
- [x] **M3 — PCI enumeration.** Brute-force config-space scan to find
      devices, specifically the virtio-gpu-pci device QEMU exposes.
- [x] **M4 — Generic virtio-pci transport.** Capability parsing, split
      virtqueues, feature negotiation — reusable by any future virtio driver
      (blk, net, ...). Needed `mm/vmm.c` (see Architecture) once testing
      showed the common-cfg BAR living way outside the HHDM.
- [x] **M5 — virtio-gpu driver.** 2D resource/scanout/transfer/flush over
      the control virtqueue; a pixel swatch proves direct framebuffer
      writes go through the driver (not Limine's boot-services
      framebuffer, which is never touched), and a small bitmap-font text
      console (`font8x8_basic`, public domain) sits on top and mirrors
      everything `kprintf` logs.

**Explicitly out of scope for now:** IRQ/MSI-X-driven virtio (everything
above polls), per-process address spaces / demand paging, virtio-blk,
virtio-net, a filesystem, a scheduler, and userspace. These are natural
next milestones from here.
