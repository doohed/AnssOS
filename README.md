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
- **Memory:** rides on Limine's higher-half direct map (HHDM) for now —
  `virtual = physical + hhdm_offset` reaches all usable RAM and MMIO regions
  (PCI BARs included), so there's no custom page-table code yet.
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
├── GNUmakefile            # builds the kernel, stages a UEFI-only ISO
├── run.sh                 # boots the ISO in QEMU with OVMF + virtio-gpu
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

Then:

```sh
make        # build kernel/bin/kernel and AnssOS.iso
./run.sh    # boot it in QEMU (UEFI/OVMF, -device virtio-gpu-pci, serial on stdout)
```

`run.sh` auto-detects the local OVMF firmware layout (combined `OVMF.fd` or
split `OVMF_CODE`/`OVMF_VARS`) and rebuilds the ISO first if the kernel
sources are newer than it.

## Roadmap

Built as a sequence of milestones. **Code-complete** means it builds clean
(`make` in `kernel/`, zero warnings under `-Wall -Wextra`) and its ELF
layout/disassembly has been checked by hand; **boot-verified** means it's
actually been run in QEMU. Everything below is code-complete; none of it is
boot-verified yet, since this sandbox is still missing `qemu-system-x86_64`/
OVMF/`xorriso` (see Building and running above) — that's the next thing to
do once those are installed.

- [x] **M0 — Boot to serial.** Limine hands off to `kmain` in 64-bit long
      mode; kernel reads the HHDM offset, memory map, boot framebuffer info,
      and RSDP pointer from Limine and logs them over COM1.
- [x] **M1 — GDT/TSS/IDT.** Own 64-bit GDT and TSS, full IDT with exception
      handlers that dump CPU state over serial instead of triple-faulting
      silently. `kmain` deliberately triggers a `#DE` (divide-by-zero) after
      setup to prove the dispatch path end-to-end.
- [x] **M2 — Physical memory manager.** Bitmap page allocator built from
      Limine's memory map, plus a contiguous-run allocator
      (`pmm_alloc_pages`) for DMA buffers like the GPU framebuffer. Self-
      tests itself (alloc/free a batch, check for dupes and for the free
      count returning to baseline) on every boot.
- [x] **M3 — PCI enumeration.** Brute-force config-space scan to find
      devices, specifically the virtio-gpu-pci device QEMU exposes.
- [x] **M4 — Generic virtio-pci transport.** Capability parsing, split
      virtqueues, feature negotiation — reusable by any future virtio driver
      (blk, net, ...).
- [x] **M5 — virtio-gpu driver.** 2D resource/scanout/transfer/flush over
      the control virtqueue; a gradient fill proves the driver (not
      Limine's boot-services framebuffer) owns the picture, and a small
      bitmap-font text console (`font8x8_basic`, public domain) sits on top
      and mirrors everything `kprintf` logs.

**Explicitly out of scope for now:** IRQ/MSI-X-driven virtio (everything
above polls), a real virtual memory manager / per-process paging,
virtio-blk, virtio-net, a filesystem, a scheduler, and userspace. These are
natural next milestones once M0–M5 are boot-verified.
