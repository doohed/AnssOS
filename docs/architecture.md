# Architecture

How AnssOS is put together, subsystem by subsystem. The
[roadmap](roadmap.md) covers *why* each piece arrived when it did and
what went wrong building it; this is the shape of the result.

## Boot and core


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
- **Drivers:** virtio, plus what a real PC needs (in progress, see
  [real-hardware.md](real-hardware.md)). The kernel enumerates PCI itself
  and speaks the virtio 1.x ("modern") PCI transport directly — no legacy
  virtio. The display is the one exception so far: with no virtio-gpu,
  `drivers/display.c` uses the firmware's GOP framebuffer that Limine
  hands over. `drivers/virtio/virtio_snd.c` (M17) is the
  newest: a virtio-sound driver wiring up just the control and tx
  virtqueues (not event/rx — playback only) for PCM output, see
  [play.md](play.md).
- **Debugging:** all kernel logging goes out over the COM1 serial port
  (`kprintf`), independent of the display, so anything after boot is
  debuggable via `-serial stdio` even before the framebuffer driver works.
  `serial_init()` checks the UART is really there (a loopback self-test)
  and goes quiet if not, as on many real PCs.

## Processes and userspace

- **Ring 3:** `arch/x86_64/usermode.c/.h/.S` is the entry/exit
  primitive -- `enter_usermode()` is a hand-rolled one-shot context
  switch (an asm-level "setjmp" of the kernel's registers, then a
  manually-built `iretq` frame into ring 3), and `return_to_kernel()`
  restores it via a bare `ret`. `TSS.RSP0` is what the CPU uses to find
  a valid ring-0 stack on any ring 3 -> ring 0 transition, and it is
  per-process state, not global.
- **Process table:** `exec/process.c` holds a fixed 16-slot
  `struct process` table (pid, parent pid, state, exit status), each
  wrapping the `struct usertask` the usermode layer needs.
- **Scheduling:** plain round-robin. `wait()` blocks by *recursively*
  calling into the scheduler (`scheduler_run_until()`) rather than
  through a suspend/resume mechanism, so there is no distinct "blocked"
  state at all. Preemption is real: `idt.c`'s `irq_handler()` notices
  the timer interrupting ring-3 code and hands the CPU to a different
  runnable process, reusing the same `arch_resume_process()` primitive
  `fork()` already needed.
- **Address spaces:** `vmm_new_address_space()` copies the current
  address space's upper 256 PML4 entries -- the whole higher half:
  kernel code, the HHDM, MMIO -- so kernel mappings are automatically
  present and supervisor-only in every task with no further
  bookkeeping. `vmm_clone_user_pages()` (for `fork()`, a full
  page-by-page copy, no copy-on-write) and `vmm_free_user_pages()` (at
  exit/exec) are its counterparts.
- **ELF loading:** `exec/elf.c` is a from-scratch static, non-PIE
  ELF64 loader. `PT_LOAD` segments each get their own
  `pmm_alloc_pages()` run, mapped `PAGE_USER`, file bytes copied in and
  the remainder zeroed for bss. It also builds the System V process
  initialization stack -- see [syscalls.md](syscalls.md#the-initial-stack).
- **Pipes (M19):** `exec/pipe.c` is a small fixed-size ring buffer, not
  vnode-based -- a pipe has no tree identity and needs ref-counted
  open/closed state. Every operation is non-blocking by design: a
  busy-spin inside a syscall handler can't yield the CPU to the *other*
  process that would need to run to produce the data being waited for,
  since preemption (above) only fires on a ring-3 interruption, never
  ring-0. `pipe()` + the narrower `use_as_stdio()` (not general
  `dup2()`) are what let tile (`userland/rust/tile/`) run independent `sh`
  processes in tiled panes -- see [syscalls.md](syscalls.md#pipes-m19)
  and [tile.md](tile.md).
- **Userland:** every program links `crt0.S` and a small hand-written
  C libc (`userland/libc.h`) over `int 0x80`. The self-test payloads are
  C; the interactive programs (`sh`, `play`, `scarf`, `tile`) are
  `no_std` Rust in one Cargo workspace, `userland/rust/`, linked the same
  way -- see [rust.md](rust.md).

## Syscalls

Vector `0x80` is the one deliberately `DPL=3` gate in the IDT, and the
only way into the kernel from ring 3 (no `SYSCALL`/`SYSRET` fast path
yet). `exec/syscall.c` dispatches on the number in `rax`. Linux's real
syscall numbers are reused throughout, which costs nothing now and
avoids a renumbering exercise if a Linux-binary-compatibility layer is
ever attempted. Full table: [syscalls.md](syscalls.md).

## Filesystem

- **`fs/vfs.c`** is a real tree of directory/file `vnode`s
  (parent/children/sibling links, absolute or cwd-relative resolution
  with `.` and `..`) living entirely in the kernel heap.
- **`fs/blkfs.c`** persists it -- deliberately not a "real" filesystem
  (no inode table, no free-space bitmap): it recursively serializes the
  *entire* `vnode` tree to a flat buffer (superblock at sector 0, tree
  data from sector 1), writes it in one shot, and replays it back
  through `vfs.c`'s own `vfs_mkdir`/`vfs_create_file`/`vfs_write_bytes`
  on load. Correct and sufficient at hobby-OS scale; not designed to
  scale to a large filesystem.
- Programs live in **`/bin`**, written there at boot from blobs
  embedded in the kernel image via `.incbin` (`exec/userland_blobs.S`)
  -- there is no host-side way to get a file onto the VFS otherwise.

## Console

Two independent output paths, both fed by `kprintf`:

- **Serial (COM1).** The primary debug console, and a real interactive
  terminal -- `run-qemu.sh` passes `-serial stdio`, so the host
  terminal *is* the console.
- **Framebuffer.** `console/fbconsole.c` draws an 8x16 bitmap font over
  whatever `drivers/display.c` found -- virtio-gpu, or the firmware's GOP
  framebuffer, drawn in a RAM copy and copied to the screen a changed
  rectangle at a time (reading video memory back is slow). The text grid
  sits inside a 16-pixel margin. Each font pixel (and the margin) is
  scaled 2x from 1600 pixels wide and 3x from 3200, so text stays
  readable on a 1080p or 4K monitor: 156x48 cells at 1280x800, 116x31 at
  1920x1080. It understands a subset
  of ANSI/CSI escape sequences: cursor addressing, erase, and SGR for the 16 ANSI
  colors (foreground and background, from its own palette), bold (drawn
  as the bright color), dim (blended toward the background) and reverse
  video -- enough for full-screen programs like [scarf](scarf.md) and
  the colored prompt of [sh](sh.md). Anything outside that subset is
  swallowed, not printed.

  The font is [Spleen](https://github.com/fcambus/spleen) 8x16 (BSD
  2-Clause, vendored in `console/spleen/`), made for terminals: each
  glyph has space above and below it in its cell, so lines of text don't
  touch. Text is UTF-8: ASCII plus 33 extra glyphs -- box drawing
  (`─ │ ┌ ╭ ├ ┼` ...), the arrow and round segment ends a powerline-style
  prompt uses (U+E0B0-U+E0B7), and `❯ ❮ ✔ ✘ · … ● █ ▌ ▐`. Any other
  character draws as `?`. `scripts/gen-font.py` generates
  `console/font8x16.h` from Spleen, plus `#`/`.` pictures of the few
  glyphs Spleen lacks; to add a glyph, add it there (and to
  `anssos_tui::console_has()`) and rerun the script. tile's per-pane
  terminal and the ratatui backend pass the same set through. A
  redraw costs one flush; on virtio-gpu that currently transfers the
  *entire* framebuffer; see [scarf.md](scarf.md#performance).

Input is equally dual: `shell.c`'s `read_line()` and the read/poll
syscalls call `input_poll_char()` (`drivers/input.c`), which polls the
virtio keyboard (needs a graphical window with focus) and COM1 on
every iteration, whichever has a byte ready. Real-hardware keyboard
drivers will join it there. Keys with no single byte --
arrows, Home/End, Delete, Page Up/Down -- come out of the virtio
keyboard as the escape sequences a VT100-style terminal sends (`ESC [ A`
for Up, `ESC [ 3 ~` for Delete, ...), so a program sees the same bytes
from either input.

The kernel boots into the userland shell, `/bin/sh` ([sh.md](sh.md)),
and falls back to the kernel-resident `shell.c` if it ever exits.
