# AnssOS

A hobby x86_64 operating system, booted via UEFI through
[Limine](https://github.com/limine-bootloader/limine), with a from-scratch
kernel that talks to hardware exclusively through **virtio** devices (QEMU
is the target platform). This is a rewrite of an earlier 2024 legacy-BIOS/
32-bit prototype — that version is still in git history, but the tree now
targets UEFI + long mode only.

## Documentation

| Document | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | how the system is put together: boot, memory, drivers, processes, syscalls, filesystem, console |
| [docs/building.md](docs/building.md) | building and running -- natively, in the container, and under UTM |
| [docs/sh.md](docs/sh.md) | the shell the system boots into: prompt, suggestions, completion, commands |
| [docs/shell.md](docs/shell.md) | the kernel-resident fallback shell, the serial console, scripted testing |
| [docs/syscalls.md](docs/syscalls.md) | the syscall table, the initial stack layout, terminal handling |
| [docs/scarf.md](docs/scarf.md) | the text editor: keys, design notes, performance |
| [docs/play.md](docs/play.md) | the WAV/MP3 player: screen, keys, MP3 decoding, the spectrum analyzer |
| [docs/tile.md](docs/tile.md) | the terminal multiplexer: panes, keys, how a pane's terminal works |
| [docs/rust.md](docs/rust.md) | the Rust userland: the Cargo workspace, how programs are built, the runtime, ratatui |
| [docs/roadmap.md](docs/roadmap.md) | milestone history -- what arrived when, and what went wrong building it |
| [docs/real-hardware.md](docs/real-hardware.md) | the plan for booting on a real PC: framebuffer, keyboard, disk, audio drivers |

## Quick start

On x86_64 Linux:

```sh
sudo apt-get install -y nasm qemu-system-x86 qemu-utils ovmf xorriso mtools python3
rustup target add x86_64-unknown-none   # Rust from https://rustup.rs, for userland/rust/
git submodule update --init --recursive
./scripts/build-iso.sh   # build userland, kernel, and assemble AnssOS.iso
./scripts/run-qemu.sh    # boot it (UEFI/OVMF, virtio-gpu-pci, serial on stdout)
```

On anything else -- an Apple Silicon Mac, say -- `make docker` builds a
container with a clang cross-toolchain and Rust, and drops you into it
with the repo mounted at `/work`; the two scripts above are still the
entry points. See [docs/building.md](docs/building.md).

It boots into [sh](docs/sh.md), a shell with a colored two-line prompt,
highlighting as you type, history suggestions and Tab completion. The
console doubles as a real serial terminal, so **typing into the terminal
you launched from talks to the system** too. Try:

```
> help
> ls /bin
> scarf .
> play testtone.wav
> tile
```


## Repo layout

```
AnssOS/
├── docs/                  # everything in the table above
├── docker/Dockerfile      # arm64-native cross-build/run environment
├── limine/                # git submodule: prebuilt Limine UEFI bootloader
├── limine.conf            # Limine boot menu, points at the kernel
├── .clang-format          # code style, enforced by scripts/format.sh
├── GNUmakefile            # thin wrapper around scripts/*.sh
├── scripts/
│   ├── build-iso.sh         # builds userland + kernel, assembles AnssOS.iso
│   ├── build-userland.sh    # builds the ring-3 programs (C and Rust), embeds them in the kernel
│   ├── run-qemu.sh          # boots an already-built AnssOS.iso in QEMU
│   ├── docker-shell.sh      # builds the container and drops into it
│   ├── format.sh            # clang-format wrapper (apply or --check)
│   ├── gen-test-tone.py     # synthesizes the testtone.wav fixture for play
│   ├── gen-font.py          # generates the console font (font8x16.h) from Spleen
│   └── disk-put.py          # writes host files straight onto AnssOS-disk.img
├── userland/              # ring-3 programs and the hand-written libc
│   ├── crt0.S               # reads argc/argv off the initial stack, calls main
│   ├── libc.h               # the whole userland API surface
│   ├── syscalls.c           # raw int 0x80 wrappers
│   ├── libc/                # string, malloc, stdio, termios, dirent
│   ├── *.c                  # hello, crash, and the self-test payloads
│   └── rust/                # Cargo workspace for the interactive programs (docs/rust.md)
│       ├── anssos/            # runtime: libc wrappers, allocator, panic handler
│       ├── tui/               # anssos-tui: the ratatui console backend
│       ├── sh/                # the shell the system boots into (docs/sh.md)
│       ├── play/              # the WAV/MP3 player (docs/play.md)
│       ├── scarf/             # the text editor (docs/scarf.md)
│       └── tile/              # the terminal multiplexer (docs/tile.md)
└── kernel/
    ├── linker.ld            # higher-half link script (Limine protocol layout)
    └── src/
        ├── main.c                     # kmain: init order, self-tests, banner
        ├── boot/                      # Limine protocol header + boot requests
        ├── arch/x86_64/
        │   ├── gdt.{c,h} idt.{c,h}      # GDT/TSS, IDT, exception + IRQ dispatch
        │   ├── fpu.{c,h}                # SSE enable, per-process FPU state
        │   ├── pic.{c,h}                # 8259 remap + LAPIC LINT0/ExtINT passthrough
        │   ├── usermode.{c,h,S}         # ring-3 entry/exit, process resume
        │   ├── isr.S irq.S syscall.S    # interrupt/syscall stubs
        │   └── io.h                     # port I/O
        ├── mm/
        │   ├── pmm.{c,h}                # bitmap physical page allocator
        │   ├── vmm.{c,h}                # page tables: MMIO, address spaces, fork/free
        │   └── heap.{c,h}               # kmalloc/kfree, first-fit over the PMM
        ├── exec/
        │   ├── elf.{c,h}                # static ELF64 loader + initial stack
        │   ├── process.{c,h}            # process table, scheduler, fork/exec/wait
        │   ├── pipe.{c,h}               # non-blocking pipes (docs/syscalls.md)
        │   ├── syscall.{c,h}            # int 0x80 dispatch (docs/syscalls.md)
        │   └── userland_blobs.{S,h}     # .incbin of the built ring-3 programs
        ├── drivers/
        │   ├── serial.{c,h}             # COM1 driver + kprintf
        │   ├── pit.{c,h} pci.{c,h}      # timer; config-space PCI enumeration
        │   ├── tty.h                    # termios/winsize layouts (Linux's)
        │   └── virtio/                  # transport, gpu, input, blk, snd
        ├── console/
        │   ├── fbconsole.{c,h}          # bitmap-font console + ANSI/CSI parser
        │   ├── splash.{c,h}             # boot splash
        │   ├── font8x16.h               # the 8x16 font, from scripts/gen-font.py
        │   └── spleen/                  # vendored Spleen 8x16 font (BSD 2-Clause)
        ├── fs/
        │   ├── vfs.{c,h}                # in-memory directory/file tree
        │   └── blkfs.{c,h}              # whole-tree persistence over virtio-blk
        ├── shell/shell.{c,h}            # kernel-resident fallback shell (docs/shell.md)
        └── lib/string.{c,h}             # freestanding mem*/str* functions
```


`make`/`make run`/`make docker`/`make format` are thin wrappers around
`scripts/*.sh`, which are the actual source of truth for how each step
works and can be run directly.
