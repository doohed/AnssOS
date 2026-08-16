# Roadmap

Built as a sequence of milestones, all **boot-verified**: booted in
QEMU (`-M q35 -vga none -device virtio-gpu-pci`, OVMF) with a
screendump confirming the console text and a pixel swatch actually
render through the virtio-gpu queue.


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

- [x] **M12 — `O_CREAT`, `mkdir`, and a per-task working directory.**
      M11's file I/O only worked against files that already existed,
      always resolved against the VFS root. `struct usertask`
      (`arch/x86_64/usermode.h`) gained a `cwd` field (always starts at
      `/` — a task doesn't inherit the launching shell's `cwd`), and two
      new Linux-numbered syscalls: `chdir` (80) and `mkdir` (83). `open`
      now accepts `O_CREAT` (`0x40`, Linux's real value) — if the path
      doesn't resolve, it creates the file via `vfs_create_file()` first
      — and resolves every path against `task->cwd` instead of always
      the root, so relative paths work the same way they already did in
      the shell's own `cwd` global. New test payload `dirtest.bin`
      proves all three together: `mkdir`s a directory, `chdir`s into it,
      `open`s a brand-new file there by a *relative* name with
      `O_CREAT`, writes and reads it back — verified by `ls`/`cat`ing it
      from the shell afterward and seeing the real thing the task
      created, not a private copy.

- [x] **M13 — a scheduler, `fork`/`exec`/`wait`, and real preemption.**
      Everything before this milestone was strictly one process at a
      time: `run` blocked the shell synchronously on a single static
      task. This is the biggest architectural jump since M10 itself,
      shipped in two halves.
      **Phase A — process table + cooperative multitasking.** New
      `exec/process.c/.h`: a fixed 16-slot `struct process` table
      (pid/parent pid/state/exit status, wrapping the same `struct
      usertask` M10 already had), and three new Linux-numbered syscalls
      — `fork` (57, full page-by-page address-space copy via new
      `vmm_clone_user_pages()`, no copy-on-write), `execve` (59,
      simplified single-arg `exec(path)`, replaces a process's image in
      place, same pid/open files/cwd), `wait4`/`getpid` (61/39,
      simplified `waitpid(pid, status)`). `wait()` blocks via an
      ordinary *recursive* call into the scheduler (`scheduler_run_until()`
      runs other processes — including the target, once picked — until
      it exits) rather than a suspend/resume mechanism; no distinct
      "blocked" state needed at all in Phase A. `shell.c`'s `run` spawns
      a process and calls `scheduler_run_until(-1)`, blocking until every
      process that launch (transitively, via `fork()`) spawned has
      exited — there's no init process to reparent orphans to yet, so
      the convention is that a process which forks a child waits for it
      before exiting.
      **Phase B — real preemption.** `idt.c`'s `irq_handler()` now
      recognizes the timer (IRQ0) interrupting ring-3 code and hands the
      CPU to a different runnable process instead of just returning —
      reusing the *same* resume primitive Phase A's `fork()` already
      needed (`arch_resume_process()`, added to `usermode.S`): a
      preempted process's full register state is already sitting on its
      own kernel stack exactly where the interrupt frame points, so
      "pausing" it costs nothing more than remembering that address.
      `isr.S`/`syscall.S`/`irq.S`'s previously-duplicated pop-registers-
      then-`iretq` tail is now one shared `common_return_from_interrupt`
      label all three (plus `arch_resume_process`) jump into. New test
      payload `preempttest.bin` proves it unambiguously: two forked
      children each spin a tight busy loop with *no* voluntary yield
      point at all, printing their own tag — the output interleaves
      (`AAABBBBBAAAABBBB...`) instead of running as two clean sequential
      blocks, which is only possible if the timer is genuinely
      interrupting one mid-flight and resuming the other.
      **Two real bugs found building this, both the same species:**
      state that's fine for exactly one task in flight breaks the moment
      a second one can be "in progress" underneath it. `wait()`'s
      recursive scheduler call dispatches a *different* process while
      the waiting one is still mid-syscall (not done, just not
      currently running) — this broke `TSS.RSP0` (left pointing at
      whichever process most recently ran, so the *outer* process's next
      syscall built its interrupt frame on the wrong kernel stack) and
      the kernel-side dispatch-resume bookkeeping (a single shared save
      slot, overwritten by the nested dispatch, stranding the outer
      process's real resume point) — both fixed by making the relevant
      state per-process (`kernel_resume` moved into `struct usertask`)
      or explicitly saved/restored around nested calls (`TSS.RSP0`,
      `usermode.c`'s `current_task`), the same `outer_current` pattern
      `process.c`'s own scheduler loop already used for itself. Caught
      by the exact symptom that pattern predicts: things worked
      perfectly for the *first* nested dispatch and broke on whatever
      ran after the recursion unwound.

- [x] **M14 — free process memory on exit/exec.** M13 made the M10-era
      "never free a process's address space or kernel stack" shortcut a
      real problem: `fork()`/`exec()`/process exit now happen
      constantly, not once per kernel lifetime. New `vmm_free_user_pages()`
      (`mm/vmm.c`) is the missing counterpart to `vmm_clone_user_pages()`
      — the same 4-level user-half walk, freeing each leaf page and
      every intermediate PT/PD/PDPT table page (never touching the
      shared higher half) back to the PMM, then the PML4 itself.
      Reap time (`exec/process.c`'s `scheduler_run_until()`, both the
      specific-pid and the auto-reap-on-empty paths) frees a zombie's
      address space and kernel stack immediately — by then `dispatch()`
      has already switched CR3/`TSS.RSP0` away, so it's provably safe.
      `exec()` is the one genuinely tricky spot: `process_exec()` runs
      *while still executing on the old kernel stack* (CR3 hasn't
      switched away yet either), so freeing either right there would
      mean freeing memory the CPU is currently using — it stashes them
      in two new `pending_free_as`/`pending_free_kstack` fields on
      `struct process` instead, freed by `process_free_pending()` at the
      start of this same process's *next* dispatch, the first point
      that's guaranteed safe. `process_fork()`'s own two OOM failure
      paths got the same treatment, so a failed `fork()` doesn't leak
      its partial clone either. Verified by running `forktest.bin` 15
      times in a row and watching `meminfo`: free pages drop once (heap
      warm-up) then stay *exactly flat* for every subsequent run — the
      actual signature of bounded memory use, not just a slower leak.

- [x] **M15 — `termios` raw mode, and `opendir`/`readdir`.** Two
      prerequisites for eventually running real terminal programs
      (`vim`'s raw keystroke-at-a-time input, directory-aware tools).
      New `drivers/tty.h` defines Linux's real `struct termios` layout
      and `ICANON`/`ECHO`/`VMIN`/`VTIME` flag values (same free-future-
      compat reasoning M10-M14 already used for syscall numbers); `struct
      usertask` gains a `termios` field, defaulted by `elf_load()` to
      today's actual behavior (`ICANON|ECHO`) and explicitly preserved
      across `exec()` — real Unix semantics: terminal settings belong to
      the terminal, not the program, `fork()` inherits it for free via
      the existing shallow copy. New syscall `ioctl` (16, `TCGETS`/
      `TCSETS=0x5401/0x5402`) gets/sets it; `sys_read_impl()`'s fd-0 path
      now branches on `ICANON` — raw mode does no echo/backspace
      handling and returns as soon as one byte is available (`VMIN=1`,
      `VTIME=0` — the one concrete combination implemented). Separately,
      `sys_open_impl()` now allows opening a directory read-only
      (`open_file.offset` reused as "index of the next child"), and new
      syscall `getdents` (217) walks `vnode->children` one entry per call
      — a deliberate simplification of real `getdents64`'s batched-
      buffer ABI. `userland/libc/termios.c` (`tcgetattr`/`tcsetattr`) and
      `libc/dirent.c` (`opendir`/`readdir`/`closedir`/`rewinddir`, a
      `DIR *` wrapping an fd, `readdir()` reusing a `static` buffer per
      real `readdir()`'s "valid until next call" contract) present the
      usual POSIX-shaped API over both. New test payloads:
      `readdirtest.bin` (lists `/`, verified against the shell's own
      `ls` — same 11 entries, same names) and `termtest.bin` (reads back
      the default `c_lflag` as `0xa` = `ICANON|ECHO`, switches to raw
      mode, and reads three interactively-typed keystrokes one at a
      time with no Enter needed and no kernel-side echo — only the
      program's own `got byte:` prints appear — before restoring the
      original settings).

- [x] **M16 -- a text editor, and the plumbing it needed.** The first
      AnssOS program that draws a full screen rather than scrolling log
      lines past: `userland/scarf.c`, a modal editor with vim
      keybindings and a file-explorer sidebar (see
      [scarf.md](scarf.md) for keys and design notes). Nothing about
      it was possible before, and the interesting part is the list of
      things that had to exist first.
      **Kernel side.** `console/fbconsole.c` gained an ANSI/CSI parser
      -- cursor addressing (`ESC[r;cH`), erase display/line, and SGR
      reverse video; before this, escape sequences were drawn as
      literal glyphs, so cursor positioning had nowhere to land.
      `exec/syscall.c` gained `TIOCGWINSZ` (nothing could ask how big
      the terminal was, though the kernel knew: `fbconsole.c` computes
      `cols = fb->width / 8`) and `O_TRUNC` (the write path only ever
      *grew* a file, so saving a shortened buffer left the previous
      version's tail behind -- which an editor does constantly).
      `drivers/virtio/virtio_input.c` gained Escape, the shifted symbol
      row, and Ctrl: all three were unreachable in a graphical window,
      which meant no way to leave insert mode, no way to type `:` for
      `:w`/`:q`, and no `Ctrl-b`. Over serial none of this ever
      mattered, because the terminal produces those bytes itself --
      the gap only existed on the virtio-input path.
      **Then `argv`, which is the real milestone.** `elf_load()` now
      builds a genuine System V process-initialization stack (`argc`,
      the `argv[]` array, its NULL, an empty `envp`, an `AT_NULL`
      auxv, and the string data), written through the HHDM since the
      stack pages are one contiguous `pmm_alloc_pages()` run. `crt0.S`
      reads them off `%rsp` and calls `main(argc, argv)`; programs
      declaring `main(void)` ignore the registers, which is why the
      ABI change didn't require touching all eleven existing payloads.
      `execve` now matches Linux's real `execve(path, argv)`, and
      threads through `process_spawn`/`process_exec`.
      Alongside it, two decisions from earlier milestones were
      reversed on purpose: a task now **inherits its launcher's cwd**
      (M12 deliberately started every task at `/`, which made `scarf .`
      meaningless), and programs moved to **`/bin`**, which `shell.c`
      searches for bare command names -- so `scarf .` works from any
      directory rather than needing `run /bin/scarf .`. New `getcwd`
      (79) resolves the inherited cwd to an absolute path. Builtins
      still win a name collision, so a file in `/bin` can never shadow
      `ls` or `cd`.
      **Three bugs worth remembering, all found by looking rather than
      reasoning.** First: a line that exactly fills the terminal width
      auto-wraps, and the explicit `\r\n` after it then advanced a
      *second* time -- every frame drifted down one row until the
      screen scrolled, leaving a duplicate status line. Caught by
      screendumping the framebuffer over QMP, not from the serial log,
      where it was invisible. Fixed by positioning every row explicitly
      with `ESC[row;colH`. Second: `.incbin` dependencies were not
      tracked -- `kernel/GNUmakefile` built `userland_blobs.S.o` from
      `%.S GNUmakefile` only, so editing a userland program and
      rebuilding silently kept the *previous* binary in the kernel
      image. That one cost two rounds of debugging a program that had
      already been fixed. Third: `opendir()` cannot detect a directory,
      because it is just `open(path, O_RDONLY)` and opening a regular
      file read-only succeeds; `chdir()` is the test that actually
      checks the type.
      Window splits (`Ctrl-w s`/`v`, a proper binary layout tree) were
      built, verified working, and then **removed**: tiled panes can't
      use `ESC[K` to clear a line without erasing the pane beside them,
      so every cell had to be padded, at roughly 20 KB of output per
      keystroke. The sidebar layout keeps one editor pane against the
      right edge specifically so it can clear with `ESC[K`.

**Explicitly out of scope for now:** making virtio interrupt-driven
(their PCI interrupt routing is a separate concern from the ISA IRQ0-15
path above), APIC/IOAPIC beyond the minimal LINT0 passthrough above (no
I/O APIC redirection table use, no SMP), a real on-disk filesystem
format (ext2/FAT/UFS — M9's format above is a whole-tree dump, not an
incremental one), copy-on-write `fork()`, `envp` for `exec()`,
dynamic linking/shared libraries, TLS, W^X/NX enforcement, the
`SYSCALL`/`SYSRET` MSR fast path (`int 0x80` only for now), signals,
`O_EXCL`/`rmdir`/`unlink` from userland,
an `init` process/orphan reparenting, priority scheduling (Phase B is
plain round-robin), and an actual musl (or similar) port capable of
building arbitrary third-party C source — the libc is still hand-
written and intentionally small, proving the pattern rather than being
generally reusable yet. These are natural next milestones
from here.
