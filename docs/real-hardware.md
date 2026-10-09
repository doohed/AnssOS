# Real hardware

The plan for booting AnssOS on a real PC instead of QEMU. The target
machine is a Ryzen 5 5600G on a MAXSUN B550M board with an RX 5600 XT,
but almost everything here applies to any modern x86_64 UEFI PC.

## Where things stand

Phase 1 is done in QEMU, waiting on the real-PC boot (step 1d): the
kernel boots to a working screen and shell with no virtio devices at
all. Everything else still only has a virtio driver:

| Need | Today | On the real PC |
|---|---|---|
| Screen | virtio-gpu, or the GOP framebuffer (`drivers/display.c`) | The RX 5600 XT's GOP framebuffer, handed over by Limine -- **works in QEMU, untested on the PC** |
| Keyboard | virtio-input, or COM1 | PS/2 if the board has a port, otherwise USB via xHCI |
| Disk | virtio-blk, blkfs from sector 0 | NVMe (or AHCI for SATA), inside a dedicated GPT partition |
| Audio | virtio-sound | Intel HD Audio controller + Realtek codec, and the GPU's HDMI/DP audio |
| Timer | LAPIC timer calibrated against the ACPI PM timer; PIT through the 8259 as a fallback (`drivers/timer.c`) | The PIT path hung the first real boot; the LAPIC timer needs no routing |
| Other interrupts | 8259 PIC through LAPIC LINT0 (unused: every driver polls) | IO-APIC or MSI-X once drivers use interrupts |
| Debug log | COM1 (`-serial stdio`), skipped when no UART answers | Only if the board has a COM header; otherwise the screen |

Things that already carry over: Limine boots over UEFI, the ISO is a
hybrid image with an EFI boot partition (so it can be written straight to
a USB stick), the PMM sizes itself from the memory map (fine for 16-64
GiB), `vmm_map_mmio()` handles BARs anywhere in the address space,
`virtio.c` already reads 64-bit BARs, and `reboot`'s triple fault works
on any x86 CPU.

Out of scope: a real `amdgpu` driver for the RX 5600 XT or the 5600G's
integrated graphics. That needs signed firmware loaded into several
on-chip processors, the display engine and power management; Linux's
version is one of the largest drivers in the kernel. The GOP framebuffer
gives a working screen at the monitor's resolution without it. No mode
switching and no acceleration is the trade-off.

## Ground rules

- **Every driver is built and tested in QEMU first.** QEMU emulates real
  controllers too -- `nvme`, `qemu-xhci` + `usb-kbd`, `intel-hda`, plain
  GOP -- so the real PC only ever runs code that already works.
- **Polling first, interrupts later**, the same as the virtio drivers.
  Polling avoids needing IO-APIC/MSI routing before anything else works.
- **Nothing writes to a real disk until the partition safety checks in
  phase 4 exist.** blkfs today writes from sector 0, which would destroy
  the partition table of whatever drive it's pointed at.
- **The virtio path stays.** Each new driver is a second implementation
  behind the same interface, picked at boot by what PCI enumeration
  finds.

## Phase 0 -- Survey the machine

Before writing drivers, find out exactly what's on the board. Boot a
Linux live USB (any distro) on the PC and save:

```sh
lspci -nnk > lspci.txt        # every PCI device with vendor:device IDs
lsusb -t   > lsusb.txt        # USB tree: which controller the keyboard is on, any hubs
sudo dmesg > dmesg.txt
sudo cat /proc/asound/card*/codec#* > codecs.txt   # HDA codec widget graph
```

Also check the board manual for a **PS/2 port** and a **COM (serial)
header**. Both make the first boots far easier. Then in the firmware
setup: **Secure Boot off** (Limine isn't signed), **CSM off** (UEFI only).

These files decide phase 3 (PS/2 or xHCI first), phase 4 (NVMe or AHCI)
and phase 6 (which codec and pins).

## Phase 1 -- Device interfaces and the GOP framebuffer

**Status: 1a-1c done and verified in `run-qemu.sh --pc`** (1920x1080 GOP,
4 GiB, no virtio devices, with and without a serial port): splash,
scrolling log, `sh`, and `scarf` full-screen. Step 1d, the first boot
on the real PC, is next. One fix beyond the plan: `serial_init()` now
checks a UART is really at COM1, because a missing one reads as an
endless stream of phantom `0xFF` key presses -- which the shell, now
running without a keyboard, would otherwise have received.

**First real boot (1d), attempt 1: hung on the splash.** The logo and
caption appeared, then nothing -- the first `timer_sleep_ms()` in the
splash animation never returned. The timer was the PIT's IRQ0 through
the 8259 and LAPIC LINT0, which QEMU wires up and this board evidently
doesn't (or the firmware left the LAPIC in x2APIC mode, where the old
MMIO setup is silently ignored -- no serial port, so no way to tell
which). Phase 5's timer item was pulled forward to fix it:
`drivers/timer.c` now runs the Local APIC timer, in xAPIC or x2APIC
mode, calibrated against the ACPI PM timer (`drivers/acpi.c`) or PIT
channel 2 polled, and checks after `sti` that ticks really arrive --
falling back to the PIT, and failing that to busy-wait sleeps, rather
than hanging. Each path was forced and verified in QEMU, and the boot
log now shows a `Timer:` line on screen saying which one is in use.

**Attempt 2: past the splash, then `vfs: out of memory` and a page
fault.** The timer worked (LAPIC, xAPIC, ARAT). The PMM returns 0 for
"out of memory", but never reserved physical page 0 -- and this PC's
firmware reports the first 640 KiB, from address 0, as usable RAM,
which QEMU's never does. So the heap's first page-run allocation got
page 0, read it as a failure, and the filesystem root was never
created. `pmm_init()` now always reserves page 0; page-fault dumps also
print `cr2` (the faulting address), and a missing VFS root stops boot
with a message instead of a fault.

The goal: boot on the PC and see the kernel log and the shell on screen.

**1a. Split the virtio assumptions out of the generic code.**

- Replace `struct virtio_gpu_fb` with a generic `struct framebuffer`
  (`pixels`, `width`, `height`, `pitch` in pixels) in
  `drivers/display.h`, flushed through `display_flush_rect()`. `fbconsole.c` and `splash.c` index with
  `y * pitch + x` instead of `y * width + x` -- a GOP framebuffer's
  pitch is often wider than its visible width.
- Add a `drivers/input.c` with `input_poll_char()` that polls every
  registered keyboard source (virtio-input now, PS/2 and USB later) and
  COM1. `shell.c`'s `read_line()` and the two polling sites in
  `syscall.c` call it instead of `virtio_input_poll_char()` +
  `serial_poll_char()`.
- In `kmain`, stop gating the VFS, `/bin` and the shell on
  `virtio_input_init()` succeeding. A machine with no keyboard driver
  yet should still boot to the shell (useful for watching boot logs).

**1b. Fall back to the GOP framebuffer.** When there's no virtio-gpu,
build the `struct framebuffer` from Limine's `framebuffer_request`
(already requested in `boot/requests.c`):

- Limine maps the framebuffer into the HHDM, so `address` is directly
  usable. Check `bpp == 32` and the red/green/blue mask shifts (16/8/0
  is the BGRX layout fbconsole already writes); refuse anything else.
- Draw into a **shadow buffer in RAM** and have `flush` copy it to the
  framebuffer. Reading video memory over PCIe is extremely slow, and
  fbconsole scrolls by reading back what's on screen. Copying only the
  rows that changed since the last flush keeps it fast at 1440p/4K.
- **Scale the font.** 8x8 glyphs at 1920x1080 is 240 columns of tiny
  text. Draw each glyph 2x (or 3x on 4K) when the framebuffer is wide.

**1c. A QEMU profile that looks like the PC.** Add an option to
`run-qemu.sh` (e.g. `--pc`) that drops every virtio device and uses
`-vga std` instead. It grows a device per phase: `-device nvme`,
`-device qemu-xhci -device usb-kbd`, `-device intel-hda -device
hda-output`, `-smp 6 -m 4G`. Boot-verify each phase there with a
screendump, the same way the roadmap milestones were verified.

**1d. First real boot.** Write `AnssOS.iso` to a USB stick (`dd`, or
balenaEtcher), boot it from the firmware boot menu, and photograph the
screen. Expected: splash, kernel log, the PCI device list, and a shell
with no keyboard yet. Compare the PCI list against `lspci.txt`.

## Phase 2 -- Debugging without a serial port

If the board has no COM header, a crash on real hardware is otherwise
invisible.

- Bring the framebuffer console up **before** PCI enumeration (the GOP
  path doesn't need PCI), so early panics land on screen.
- Keep a kernel log ring buffer from the first `kprintf`, replayed onto
  the screen once fbconsole is up, and add a `dmesg` builtin.
- Make the exception handler's register dump fit on one screen, and stop
  there instead of scrolling it away.

## Phase 3 -- Keyboard

**3a. PS/2 (i8042)**, about 150 lines: init the controller, enable the
keyboard port, register IRQ1 with `irq_register()` (the PIC path already
works), translate scan code set 1 to the same bytes and escape sequences
virtio-input produces. Works with a PS/2 keyboard, and sometimes with a
USB keyboard when the firmware's "USB legacy support" emulates one --
convenient for early boots, not something to rely on.

**3b. xHCI + USB HID keyboard**, the biggest driver in this plan (a few
thousand lines):

1. Find every controller with class `0C.03` prog-if `30`. The 5600G and
   the B550 chipset each have their own, so drive all of them.
2. **BIOS handoff** through the USB Legacy Support extended capability.
   Skipping this leaves the firmware's SMM code fighting the driver.
3. Reset the controller; set up the device context base array, the
   command ring, and one event ring (polled).
4. For each connected port: reset it, Enable Slot, Address Device, read
   the device and configuration descriptors.
5. For a HID keyboard: Set Configuration, Set Protocol (boot protocol),
   then poll its interrupt-IN endpoint for 8-byte boot reports and turn
   key-down transitions into bytes, plus key repeat driven by the PIT.

First version supports a keyboard plugged **directly** into a rear port.
USB hubs (including ones inside monitors, and some front-panel wiring)
come after, as a follow-up.

## Phase 4 -- Disk

**4a. A block-device interface.** `struct block_device` with `read`,
`write`, `sector_count` and a `first_sector` offset. blkfs goes through
it instead of calling `virtio_blk_*` directly, and every access is
bounds-checked against the device's range.

**4b. GPT and the AnssOS partition.** On a real disk, blkfs only ever
uses a GPT partition with an AnssOS-specific type GUID. No such
partition means in-memory only, never "use the whole disk". The
partition is created from Linux or Windows by shrinking an existing one
(`gdisk`/`parted` with the custom type GUID, documented here when
chosen). virtio-blk in QEMU keeps the whole-disk layout, so
`AnssOS-disk.img` and `scripts/disk-put.py` keep working.

**4c. NVMe driver**, roughly 600-800 lines: enable bus mastering, map
BAR0 (64-bit), reset, create the admin queues, Identify controller and
namespace, create one I/O submission/completion queue pair, and do
reads/writes with PRP entries (PRP lists for transfers over two pages).
Polled completion.

**4d. Read-only first.** Ship with writes disabled and an `lsblk`
builtin that prints the GPT. Turn writes on only after the partition
bounds are confirmed on the real disk.

If the PC boots from a SATA SSD instead, the driver is AHCI. Comparable
size, same interface.

## Phase 5 -- Platform

Not strictly required for the phases above, but they make the system
behave correctly rather than by luck:

- **ACPI tables:** `drivers/acpi.c` already walks the XSDT/RSDT for the
  FADT's PM timer; still to read: MADT for the IO-APIC and the CPU list,
  the FADT's reset register.
- **IO-APIC** instead of the 8259 through LINT0, or **MSI-X** per device
  once drivers move from polling to interrupts.
- **Timer:** done early, see phase 1d above -- the LAPIC timer, which
  SMP will need per CPU anyway.
- **SMP** (using all 6 cores of the 5600G) is a project of its own --
  the scheduler and every shared kernel structure assume one CPU.

## Phase 6 -- Audio

An Intel HD Audio driver behind an `audio_open/write/close` interface
that `play`'s syscalls already match (S16LE, 44.1/48 kHz, stereo):

1. Find controllers with class `04.03`. The board's analog audio and the
   RX 5600 XT's HDMI/DP audio are separate controllers.
2. Reset, set up the CORB/RIRB command rings, enumerate codecs.
3. Walk the codec's widget graph from an output pin (line out or
   headphones on the board; the display pin on the GPU) back to a DAC,
   set amplifier gains and unmute along the path.
4. One output stream with a buffer descriptor list. Pace `write` with
   the stream's position register, which is what makes playback run in
   real time, as `virtio_snd_write()` does today.

Roughly 1000-1500 lines. `codecs.txt` from phase 0 shows the codec's
exact widget layout, which makes step 3 much easier to get right.

## Later

- Network: the board's Ethernet controller (check `lspci.txt`; usually a
  Realtek RTL8111-family part).
- USB hubs, USB mass storage (boot stick as a writable disk), mouse.
- ACPI power-off (needs an AML interpreter -- large).

## Order and size

| Phase | Result on the PC | Size |
|---|---|---|
| 0 | Device list, firmware set up | an evening |
| 1 | Boots to a readable screen | small |
| 2 | Crashes are visible | small |
| 3a | Typing works (PS/2 or legacy emulation) | small |
| 3b | Typing works with any USB keyboard | large |
| 4 | Files persist on the NVMe | medium |
| 5 | Correct interrupt routing, ACPI reset | medium |
| 6 | `play` works through speakers or HDMI | medium |
