# Building and running


Needs `nasm`, `qemu-system-x86_64`, `qemu-img`, OVMF firmware, `xorriso`,
`mtools`, and `python3` (`scripts/gen-test-tone.py`/`scripts/disk-put.py`
synthesize/inject audio assets -- see [play.md](play.md) -- and need real
binary struct packing and float math bash doesn't have):

```sh
sudo apt-get install -y nasm qemu-system-x86 qemu-utils ovmf xorriso mtools python3
```

Building and running are two separate scripts (`make`/`make run` are thin
wrappers around them, if you'd rather not call them directly):

```sh
./scripts/build-iso.sh   # build the userland test payloads, kernel/bin/kernel, and assemble AnssOS.iso
./scripts/run-qemu.sh    # boot the already-built ISO (UEFI/OVMF, virtio-gpu-pci, serial on stdout)
```

`run-qemu.sh` does **not** build anything itself — it errors out if
`AnssOS.iso` doesn't exist yet, telling you to run `build-iso.sh` first.
It auto-detects the local OVMF firmware layout (combined `OVMF.fd` or split
`OVMF_CODE`/`OVMF_VARS`). It also creates a blank 16 MiB `AnssOS-disk.img`
next to the ISO on first run (via `qemu-img`, gitignored) and attaches it
as `virtio-blk-pci` for the M9 persistent filesystem — delete the file to
reset storage back to blank.

## Building on a host that isn't x86_64 Linux

The `apt-get` line above assumes an x86_64 Linux host, where `cc`/`ld`
already are the right toolchain. Everywhere else — an Apple Silicon Mac
being the case this was set up for — `docker/Dockerfile` supplies the
environment instead, and `make docker` builds it and drops you into a
shell with the repo bind-mounted at `/work`:

```sh
make docker                       # or ./scripts/docker-shell.sh
# then, inside:
git submodule update --init --recursive   # first time only (Limine)
./scripts/build-iso.sh
./scripts/run-qemu.sh
```

The container is an environment, not a second build system — the two
scripts above are still the entry points, unchanged. Nothing in the image
is emulated either: it's a native arm64 Debian, pinned to a dated tag so
the toolchain can't drift, because both halves of the problem can be
solved without a foreign-arch userland. clang is a cross-compiler by
construction and will emit x86_64 given `--target=x86_64-unknown-none`
(so no cross-gcc to build or install, and `ld.lld` links the result with
the same `-m elf_x86_64` the GNU `ld` invocation already used), while
`qemu-system-x86_64` emulates the guest CPU in TCG no matter what it's
running on. TCG means no HVF/KVM acceleration — boot takes a second or
two rather than being instant, which for a kernel this size is not worth
caring about.

`CC`/`LD` come from the image's `ENV`, so nothing needs configuring
inside it; `kernel/GNUmakefile` and `scripts/build-userland.sh` both
default to `cc`/`ld` when those aren't set and only add the `--target`
flag when the compiler actually reports itself as clang, so a native
x86_64 gcc build produces the exact same command line it did before any
of this existed. The `ovmf` package is worth a note too: it's
`Architecture: all` in Debian (the firmware is x86 machine code, but it's
shipped as data, not as an executable the host has to run), so it
installs on arm64 unchanged and lands in `/usr/share/OVMF/` as the split
`OVMF_CODE_4M.fd`/`OVMF_VARS_4M.fd` pair that `run-qemu.sh`'s existing
auto-detection already looks for first.

The container runs as root, so files it creates in the bind mount are
root-owned. On Docker Desktop for Mac that's invisible — the VirtioFS
mount maps ownership back to you — but on a Linux host you'll want to
`chown` build output afterwards or run the container with `--user`.

## Code style

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

## Running under UTM

UTM is a GUI wrapper around the same QEMU, but its defaults differ from
what `run-qemu.sh` passes, and AnssOS is virtio-only by design -- it has
no PS/2 or USB HID driver, so UTM's default input devices are invisible
to it. In VM Settings:

- **System -> Machine:** `q35`, with **UEFI Boot** enabled. There is no
  legacy BIOS path.
- **Devices -> Display:** `virtio-gpu-pci` specifically. `main.c` looks
  for PCI class 3.80; `virtio-vga` presents as a VGA-class device and
  will not be found, leaving you in the `Skipping M4/M5` branch.
- **QEMU -> Arguments:** add `-device` and `virtio-keyboard-pci` as two
  separate entries (each row is one argv token). Without a virtio
  keyboard, `virtio_input_init()` fails and `main.c` skips the shell
  entirely -- the kernel halts after printing why.
- **Devices -> New -> Serial.** Not optional in practice: with no UART
  at COM1, reads of the Line Status Register return `0xFF`, whose
  bit 0 reads as "data ready" forever, so the shell receives an endless
  stream of phantom `0xFF` bytes.
