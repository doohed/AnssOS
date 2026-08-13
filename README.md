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
        ├── arch/x86_64/idt.{c,h}     # IDT + exception/IRQ dispatch (isr.S/irq.S have the stubs)
        ├── arch/x86_64/pic.{c,h}     # 8259 remap + Local APIC LINT0/ExtINT passthrough
        ├── mm/pmm.{c,h}              # bitmap physical page allocator
        ├── mm/vmm.{c,h}              # minimal page-table mapper, MMIO only
        ├── mm/heap.{c,h}             # kmalloc/kfree, first-fit over the PMM
        ├── drivers/serial.{c,h}      # COM1 driver + kprintf
        ├── drivers/pit.{c,h}         # PIT timer: ticks/uptime/sleep_ms
        ├── drivers/pci.{c,h}         # legacy config-space PCI enumeration
        ├── drivers/virtio/virtio.{c,h}      # virtio-pci transport + virtqueues
        ├── drivers/virtio/virtio_gpu.{c,h}  # virtio-gpu 2D driver
        ├── drivers/virtio/virtio_input.{c,h} # virtio-input keyboard driver
        ├── console/fbconsole.{c,h}   # bitmap-font text console over the gpu framebuffer
        ├── console/splash.{c,h}      # boot splash: centered logo + looping "..." indicator
        ├── console/font8x8_basic.h   # vendored public-domain 8x8 font (dhepper/font8x8)
        ├── shell/shell.{c,h}         # interactive command shell
        ├── fs/vfs.{c,h}              # in-memory directory/file tree
        └── lib/string.{c,h}          # freestanding mem*/str* functions
```

## Building and running

Needs `nasm`, `qemu-system-x86_64`, `qemu-img`, OVMF firmware, `xorriso`,
and `mtools`:

```sh
sudo apt-get install -y nasm qemu-system-x86 qemu-utils ovmf xorriso mtools
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
      everything `kprintf` logs. `console/splash.c` shows a Fedora/
      Windows-style boot splash first (centered ASCII logo, looping
      `.`/`..`/`...` indicator) before the console takes over — paced with
      `pit_sleep_ms()` (see M8) as of that milestone, a plain busy-wait
      spin before it.
- [x] **M6 — virtio-input keyboard + interactive shell.** QEMU exposes
      keyboard/mouse/tablet under the identical PCI id, so
      `virtio_input_init()` walks every match (`pci_find_device_nth()`)
      and reads each one's `VIRTIO_INPUT_CFG_ID_NAME` to find the one
      actually named "Keyboard". `shell/shell.c` reads lines from it
      (polling, with backspace editing) and dispatches to a small builtin
      table (`help`, `echo`, `clear`, `uname`, `meminfo`, `lspci`,
      `crash`, `reboot`, `halt`) — command *names* are matched case-
      insensitively, arguments keep whatever case you typed. The `crash`
      builtin is the `#DE` self-test that used to run automatically at
      the end of boot — now on demand, since the exception handler halts
      forever and we want an interactive prompt afterward instead.
      `read_line()` also polls `serial_poll_char()` (`drivers/serial.c`)
      alongside virtio-input, so the shell works as a real serial console
      too — added after virtio-input alone turned out to need a
      graphical window with keyboard focus to receive anything at all,
      which a headless `-display none` run (this repo's default) simply
      doesn't have. See "Using the shell interactively" below.
- [x] **M7 — In-memory filesystem + directory/file management.** Two new
      pieces underneath the shell: `mm/heap.c` (`kmalloc`/`kfree`, a
      first-fit allocator carving variable-sized blocks out of pages from
      `pmm_alloc_pages()` — nothing above the PMM could allocate anything
      but whole pages before this), and `fs/vfs.c`, a real tree of
      directory/file `vnode`s (parent/children/sibling links, absolute or
      cwd-relative path resolution with `.`/`..`) that lives entirely in
      that heap and resets on reboot — no block device or on-disk format
      yet (that's the natural next step, once this UX layer was proven).
      New shell builtins, deliberately not named like standard Unix
      (`create dir <name>` instead of `mkdir`, `create file <name>`
      instead of `touch`, same verb-first pattern for `delete`/`copy`/
      `move`): `create`, `delete`, `copy`, `move` (each `dir`/`file` +
      path, `copy`/`move` also take a destination), plus `cd`, `pwd`,
      `ls`, `cat`, `write` to navigate and inspect. The prompt now shows
      the current directory (`AnssOS:/some/path>`). `vfs_remove()`
      refuses to delete the current directory or an ancestor of it (the
      shell's `cwd` pointer would otherwise dangle) — verified by trying
      to delete `.`, `..`, and an absolute ancestor path while `cwd` sat
      inside the target, all correctly refused.
- [x] **M8 — Hardware interrupts + a PIT timer.** Everything before this
      milestone polled; there was no clock, no `sleep()`, nothing a future
      scheduler could tick against. `arch/x86_64/pic.c` remaps the 8259
      pair (IRQ0-15 -> vectors 32-47, clear of the CPU exception range),
      `arch/x86_64/irq.S` + `idt.c` add a generic 16-line IRQ dispatch
      (`irq_register()`) alongside the existing exception handlers, and
      `drivers/pit.c` programs channel 0 for a 100 Hz tick
      (`pit_ticks()`/`pit_uptime_ms()`/`pit_sleep_ms()`, the last now
      pacing the M5 splash instead of a guessed spin count; new `uptime`
      shell builtin).
      **The actual discovery this milestone was about:** a fully-correct
      PIC remap + PIT program + unmask + `sti` still left every timer
      interrupt permanently stuck pending in the 8259's IRR, never
      delivered — confirmed via QEMU's `info pic` (`irr=01`, `isr=00`,
      CPU confirmed halted with IF=1). On chipsets that default to
      APIC-only routing (QEMU's q35 included), the legacy PIC's INTR line
      only reaches the CPU if the Local APIC's LINT0 pin is explicitly
      configured for ExtINT delivery and unmasked, and the LAPIC itself
      software-enabled via its Spurious Interrupt Vector Register — none
      of which firmware guarantees once it's done with its own boot phase.
      `pic_remap()` now also does this (`lapic_enable_extint_passthrough()`
      in `pic.c`), which is why it has to run after `pmm_init()` (it maps
      the LAPIC's MMIO page via `vmm_map_mmio()`) rather than right after
      `idt_init()` like the rest of CPU bring-up. Also caught while wiring
      up the `uptime` command: `kprintf` has no field-width support
      (`%03lu` silently consumed zero arguments instead of erroring,
      desyncing every `va_arg` read after it in that call) — its own doc
      comment already said so, but it's a sharp edge worth remembering.

- [x] **M9 — Persistent storage (virtio-blk).** The M7 filesystem was
      100% in-RAM and reset every reboot. `drivers/virtio/virtio_blk.c`
      is a new virtio-blk-pci driver (device type 2, PCI id `0x1042`),
      same shape as the gpu/input drivers: built entirely on the
      existing generic transport in `virtio.c`, one request virtqueue,
      each I/O a 3-descriptor chain (header / data / status).
      `fs/blkfs.c` is the persistence layer on top of it — not a "real"
      filesystem (no inode table, no free-space bitmap): it recursively
      serializes the *entire* in-memory `vnode` tree to a flat buffer
      (superblock at sector 0, tree data from sector 1) and writes it in
      one shot, replaying it back through `vfs.c`'s own
      `vfs_mkdir`/`vfs_create_file`/`vfs_write_bytes` on load — the same
      pragmatic "correct and sufficient at hobby-OS scale, not designed
      to scale to a large filesystem" trade-off this project has made
      elsewhere (splash pacing, the shell's line editor). New
      `vfs_write_bytes()` in `vfs.c` (a `vfs_write_file()` refactor)
      takes an explicit length instead of `strlen()`-ing, since restored
      file content isn't NUL-terminated text. The shell auto-saves after
      every successful `create`/`delete`/`copy`/`move`/`write`, so
      persistence needs no extra step; a new `sync` builtin flushes
      on demand (e.g. right before `halt`/`reboot`). Booting without
      `-device virtio-blk-pci` attached still works exactly as in M7 —
      in-memory only, `sync` reports there's nothing to persist to.

- [x] **M10 — Ring-3 userspace (one program at a time).** Everything
      before this milestone ran as one privileged kernel blob; nothing
      could execute code the kernel didn't ship. `mm/vmm.c` grew a real
      per-address-space API (`vmm_new_address_space()`/`vmm_map()`/
      `vmm_switch()`, plus a `PAGE_USER` flag) on top of its existing
      4-level page-table walker — a fresh address space copies the
      current one's upper 256 PML4 entries (the whole higher half:
      kernel code, the HHDM, MMIO) so kernel/HHDM mappings are
      automatically present, supervisor-only, in every task without any
      further bookkeeping. `arch/x86_64/gdt.c`'s ring-3 GDT descriptors
      (built since early on but never used) are now exposed as
      `SEL_UCODE`/`SEL_UDATA`, and `tss_set_kernel_stack()` wires up
      `TSS.RSP0` — previously never set at all, which is what the CPU
      needs to find a valid ring-0 stack on any ring 3 -> ring 0
      transition. New `arch/x86_64/usermode.c/.h/.S` is the actual
      ring-3 entry/exit primitive: `enter_usermode()` is a hand-rolled
      one-shot context switch (asm-level "setjmp" of the kernel's own
      registers/`rsp`, then a manually-built `iretq` frame into ring 3);
      `return_to_kernel()` (called from the exit syscall, and from a
      user-mode fault -- see below) restores it via a bare `ret`. New
      vector `0x80` (the one deliberately `DPL=3` gate in the IDT) is
      the syscall entry point, dispatching in new `exec/syscall.c` to
      `exit`/`write`/`read` (Linux's own syscall numbers, reused for
      free in case a real Linux-binary-compatibility layer is ever
      attempted). `exec/elf.c` is a from-scratch static, non-PIE ELF64
      loader: `PT_LOAD` segments get their own `pmm_alloc_pages()` run,
      mapped `PAGE_USER`, file bytes copied in and the rest zeroed for
      bss. New shell builtin `run <path>` ties it together. A hand-
      rolled 3-syscall "libc" + two tiny test programs live in the new
      `userland/` directory (`hello.c`, proving `write`/`read`/`exit`
      round-trip; `crash.c`, deliberately faulting through a null
      pointer) — built by `scripts/build-userland.sh` and embedded
      directly into the kernel image via `.incbin` (see
      `exec/userland_blobs.S`) as self-test fixtures, the same idea as
      the PMM/VMM self-tests above but landing on the VFS instead of
      just printing a result.
      **The actual discovery this milestone was about:** `idt.c`'s
      `struct interrupt_frame` had every general-purpose register field
      declared in the *same* order the assembly stubs `push` them in —
      but since `push` stores at the stack pointer *after* decrementing
      it, the last register pushed ends up at the lowest address (offset
      0), not the first. The struct needed the reverse order to actually
      alias the pushed registers correctly (the doc comment even said
      "reversed" — just not what the code did). This bug predates M10
      by nine milestones and was completely invisible the whole time,
      since the only consumer (`isr_handler`'s fatal-exception dump) only
      ever *printed* these values after an unrecoverable halt — nobody
      was cross-checking a frozen crash dump against what the registers
      "should" say. It became fatal the moment `syscall_dispatch()`
      started reading `frame->rax`/`rdi`/`rsi`/`rdx` for their actual,
      load-bearing meaning (the syscall number and arguments): every
      syscall silently read the wrong register, always fell through to
      the "unknown syscall" case, and returned without doing anything —
      which looked exactly like a hang, since the userland test payload
      never even checks `write()`/`exit()`'s return values.
      Diagnosed by querying real CPU state via QEMU's monitor (`info
      registers`) rather than trusting kernel-side debug prints, which
      is what actually revealed `RAX`/`RDI` held plausible values while
      `struct interrupt_frame`'s fields didn't reflect them.

- [x] **M11 — a real userland libc.** M10 proved ring-3 execution but
      the only thing that could run was a hand-written `crt0.S` + a
      3-function stub — no dynamic memory, no `printf`, no way to touch a
      real file beyond fd 0/1/2. A genuine `musl` port isn't realistic
      yet (it expects a fully Linux-compatible syscall surface — real
      `mmap`, `clone`, `futex`, `arch_prctl` for TLS — and a TLS ABI
      AnssOS has none of), so this milestone is a small, from-scratch
      libc instead: enough for a hand-written C program to allocate
      memory, format output, and read/write a real VFS file.
      `struct usertask` (`arch/x86_64/usermode.h`) gained a brk-managed
      heap range (`heap_start`/`heap_end`) and a small fixed-size open-
      file table (`MAX_OPEN_FILES=8`), plus a `usermode_current_task()`
      accessor so `exec/syscall.c` can reach the running task's state —
      previously nothing did, since M10 never needed mutable per-task
      state beyond what it took to launch a task once.
      Four new syscalls, all Linux-numbered like M10's original three:
      `brk` (grows the heap a page at a time via `pmm_alloc_page()` +
      `vmm_map()`, capped at 4 MiB, never shrinks — same "no giving
      memory back" pragmatism as `mm/heap.c`'s own `kmalloc`/`kfree`),
      and `open`/`close`/`lseek` (`O_RDONLY`/`O_WRONLY` only, no
      `O_CREAT` — a task can only work with files that already exist).
      `read`/`write` gained an `fd >= 3` path through the new file table,
      writing straight into `vnode->data` (growing it via a fresh
      `kmalloc`+`memcpy`+`kfree` when a write extends past the current
      end — the same operation `fs/vfs.c`'s own `vfs_write_bytes()` does
      for a whole-file overwrite, just done here directly since this
      needs to write at an arbitrary offset, matching `fs/blkfs.c`'s
      existing precedent of touching vnode fields directly).
      The libc itself lives in `userland/libc/` (`string.c`, `stdio.c`,
      `malloc.c`) plus `userland/syscalls.c` (raw `int 0x80` wrappers)
      behind a shared `userland/libc.h`. `malloc()`/`free()` is a
      from-scratch free-list allocator *deliberately mirroring*
      `mm/heap.c`'s own design (first-fit, forward-only coalescing,
      grows on demand — here via `brk()` instead of `pmm_alloc_pages()`)
      rather than reusing its code, since userland can't call kernel
      functions. `printf()` re-implements `kprintf`'s exact minimal
      format-spec support over `write(1, ...)`. Two new self-test
      payloads prove it: `malloctest.bin` (several `malloc()`s of
      different sizes, a pattern-fill/verify pass, a `free()`+reuse
      check) and `filetest.bin` (`open`/`read`/`printf`s a real VFS
      fixture file, then `lseek`s and `write`s to patch and extend it —
      verified by `cat`ing the file from the shell before and after and
      confirming the *shell* sees the change the ring-3 task made,
      proving the syscall reaches the real VFS, not a copy).

**Explicitly out of scope for now:** making virtio interrupt-driven
(their PCI interrupt routing is a separate concern from the ISA IRQ0-15
path above — everything still polls), APIC/IOAPIC beyond the minimal
LINT0 passthrough above (no I/O APIC redirection table use, no SMP), a
real on-disk filesystem format (ext2/FAT/UFS — M9's format above is a
whole-tree dump, not an incremental one), `fork()`/multiple concurrent
processes, any preemptive scheduling, dynamic linking/shared libraries,
TLS, W^X/NX enforcement, the `SYSCALL`/`SYSRET` MSR fast path (`int
0x80` only for now), signals, `O_CREAT`/directory creation from
userland, a per-task working directory, and an actual musl (or similar)
port capable of building arbitrary third-party C source — M11's libc is
still hand-written and intentionally small, proving the pattern rather
than being generally reusable yet. These are natural next milestones
from here.

## Using the shell interactively

`./scripts/run-qemu.sh` defaults to `-display none` (no graphical window
at all — `virtio-gpu`/`virtio-keyboard` still work, there's just nothing
on screen and no keyboard focus to give them). That's deliberate: the
shell doubles as a real **serial console**. `drivers/serial.c`'s
`serial_poll_char()` reads bytes straight off COM1, and
`shell/shell.c`'s `read_line()` polls it alongside
`virtio_input_poll_char()` — whichever has a byte ready wins. Since
`run-qemu.sh` passes `-serial stdio`, that means **typing directly into
the terminal you ran the script from talks to the shell**, no graphical
window, VNC, or anything else required. This is the normal way to use
AnssOS in this repo.

If you do want a graphical window instead (to exercise the
virtio-gpu/virtio-input path specifically, e.g. while working on those
drivers), add a real display backend yourself, e.g.
`./scripts/run-qemu.sh -display gtk` (or `sdl`) — click into that window
for keyboard focus; the terminal you launched it from goes back to being
just the serial log in that case.

## Testing keyboard input headlessly (scripted, non-interactive)

For driving the shell from a script without occupying a terminal (as
opposed to a person just typing, which the serial console above already
covers): QEMU's HMP `sendkey` monitor command does **not** reach
`virtio-keyboard-pci` in this setup — it targets the legacy PS/2 keyboard
that q35's i8042 controller provides by default (`info qtree` shows both
`ps2-kbd` and `virtio-keyboard-pci` present simultaneously), and AnssOS has
no PS/2 driver on purpose (virtio-only). Use QMP's `input-send-event`
instead, with `device` set to the **display** device's id (not the
keyboard's — that crashes this QEMU build), e.g.
`-device virtio-gpu-pci,id=gpu0` and `{"execute": "input-send-event",
"arguments": {"device": "gpu0", "events": [...]}}` for each key down/up.
(Simplest of all for scripting, though: just write to the QEMU process's
stdin like a person would type, since `-serial stdio` is listening —
no QMP needed if the serial console path above is good enough for what
you're testing.)

One more gotcha specific to the QMP/virtio-input path: punctuation keys
use QEMU's `QKeyCode` names, not the literal character -- `.` is `"dot"`,
`/` is `"slash"`, `,` is `"comma"`, etc. Sending the raw character for
these silently fails (check the JSON response for an `"error"` key; it's
easy to miss otherwise) and the keystroke is just dropped, which reads
exactly like a kernel bug until you notice `main.c` arrived as `mainc`.

Scripting the serial console via a FIFO (`qemu-system-x86_64 ... -serial
stdio < some.fifo`) has its own gotcha: each `printf ... > some.fifo`
from a separate shell invocation opens and immediately closes the write
end. Every close-with-no-other-writer-open briefly signals EOF to
whatever's reading the pipe, and QEMU's stdio backend treats that as "the
user hit Ctrl-D" and stops polling stdin for the rest of the process's
life -- keystrokes sent afterwards vanish with no error. Keep a single
write descriptor open for the whole session instead (`exec 4<>some.fifo`
once, `printf ... >&4` for every subsequent command, from the *same*
shell process) and it behaves like a real terminal.

`virtio-blk-pci` is also worth calling out: unlike `virtio-gpu-pci`/
`virtio-keyboard-pci` (which QEMU only ever exposes as modern virtio
1.x devices), `virtio-blk-pci` defaults to the legacy/transitional PCI
device id (`0x1001`) unless you pass `disable-legacy=on`. `virtio_blk.c`
only looks for the modern id (`0x1042`, see `VIRTIO_BLK_PCI_DEVICE_ID`),
so without that flag `virtio_blk_init()` reports "no block device
found" even though `-device virtio-blk-pci` is right there on the
command line. `scripts/run-qemu.sh` already passes it; add it yourself
if you ever invoke `qemu-system-x86_64` directly.
