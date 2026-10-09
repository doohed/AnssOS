# Rust userland

The interactive programs are written in Rust: [sh](sh.md), [play](play.md),
[scarf](scarf.md) and [tile](tile.md). Each started as C and was
rewritten (M23-M26, see the [roadmap](roadmap.md)). The C side of
userland is still there underneath them: `crt0.S`, the hand-written libc
(`userland/libc.h`, `syscalls.c`, `libc/`) and the small self-test
programs (`hello.c`, `forktest.c`, `pipetest.c`, ...).

## The workspace

AnssOS has no Rust standard library, so every Rust program is
`#![no_std]`. They live in one Cargo workspace, `userland/rust/`, with
one lockfile and one `target/`:

```
userland/rust/
  Cargo.toml          workspace: members, shared deps, release profile
  .cargo/config.toml  target = x86_64-unknown-none
  anssos/             the runtime every Rust program links
  tui/                anssos-tui: ratatui on the AnssOS console
  sh/                 the shell the system boots into
  play/               the WAV/MP3 player
    c/mp3.c             play's own C: compiles minimp3's implementation
    vendor/minimp3/     minimp3, vendored unmodified (CC0)
  scarf/              the vim-style editor
  tile/               the terminal multiplexer
```

| Crate | Depends on | Draws with |
|---|---|---|
| `sh` | `anssos` | plain escape codes: a shell scrolls, it doesn't own the screen |
| `play` | `anssos`, `anssos-tui`, ratatui | ratatui |
| `scarf` | `anssos`, `anssos-tui`, ratatui | ratatui |
| `tile` | `anssos`, `anssos-tui`, ratatui | ratatui, plus a terminal emulator per pane |

## How a program is built

Each program is a **static library** for `x86_64-unknown-none`, not a
Rust binary. `scripts/build-userland.sh` builds the whole workspace with

```sh
cargo build --release \
    --manifest-path userland/rust/Cargo.toml \
    --config userland/rust/.cargo/config.toml
```

which works from any directory (plain `cargo build --release` inside
`userland/rust/` does the same). It then links each `lib<name>.a` with
`crt0.o` and the C libc, using the same `link.ld` as every C program, and
embeds the ELF in the kernel like any other (`/bin/<name>` at boot).
`crt0` calls the `main` the program exports with
`#[unsafe(no_mangle)] extern "C"`, just as it would a C program's.

A program's own C (only `play` has any: `c/mp3.c`) is compiled by the
build script, not by Cargo, so it gets exactly the same `CC`/`CFLAGS` as
the rest of userland.

The build needs a Rust toolchain plus the target (see
[building.md](building.md)); the first build downloads ratatui and its
dependencies from crates.io.

**Adding a program** is a new workspace member that depends on
`anssos`, plus one `build_program` line in `build-userland.sh`.

## The `anssos` runtime

`anssos/` holds everything a `no_std` program still needs:

- **Safe wrappers over the C libc**, never the kernel directly: files
  (`File`, closed on drop), raw mode (`RawMode`, restored on drop),
  directories (`read_dir`, `chdir`/`getcwd`, `mkdir`), the file
  operations (`unlink`, `rename`, `copy`, `sync`), processes and pipes
  (`fork`, `exec`, `waitpid`, `pipe`, `use_as_stdio`), the window size,
  the clock, keys (`read_key`, `poll_key`) and audio (`Audio`).
- **Pipe-aware I/O.** Inside a tile pane, stdin and stdout are pipes,
  which never block. `read_key()` and `write_all()` call `sched_yield()`
  and retry instead of spinning, so a program works the same on the
  console and in a pane (see [tile.md](tile.md#sharing-the-cpu)).
- **The global allocator**, on the libc's `malloc()`/`free()`. That
  `malloc` only promises its own alignment, so every block is
  over-allocated and the original pointer is stashed just below the
  aligned one.
- **The panic handler**, which resets console attributes, shows the
  cursor, prints the panic and exits with code 101.

## Ratatui without std

Ratatui 0.30 supports `no_std` (`default-features = false`; it needs
`alloc`). Its built-in backends (crossterm and the rest) need std, so
`anssos-tui` (`tui/src/backend.rs`) implements
`ratatui::backend::Backend` for the AnssOS console. It queues output into
one buffer and writes it with a single `write()` per frame, and emits
only what the console understands (see
[architecture.md](architecture.md#console)):

- CUP, ED and EL, with CUP skipped for consecutive cells on a row;
- SGR for the 16 ANSI colors (foreground and background), bold, dim and
  reverse video. Indexed and RGB colors map to the nearest of the 16;
  other modifiers are dropped;
- printable ASCII plus the console's extra glyphs
  (`anssos_tui::console_has()`), sent as UTF-8. Any other symbol is
  mapped to a stand-in.

Ratatui diffs each frame against the previous one, so only changed cells
are sent. `anssos_tui::init()` clears the screen and hides the cursor
(programs draw their own as a solid cell); `restore()` brings back normal
video, a cleared screen and a visible cursor on exit. `solid()` is the
one shared style: reverse video, which on a space is a solid cell.

**The bottom row stays empty.** The console wraps the cursor as soon as
a glyph lands in the last column; it has no deferred wrap. Writing the
bottom-right cell would therefore scroll the whole screen up a line, so
the backend reports one row fewer than the console has.

**Soft-float.** `x86_64-unknown-none` is a soft-float target, so any
float math in Rust code runs in software. The programs avoid it on hot
paths: `play` keeps its PCM, volume and spectrum math integer and lays
out with plain `Rect` arithmetic instead of ratatui's float-based
constraint solver. C code (minimp3) is compiled with SSE and uses the
FPU, which is safe because the kernel saves and restores each process's
FPU/SSE state.
