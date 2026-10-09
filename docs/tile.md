# sh and tile

Two pieces (M19-M21, tile rewritten in M25) that together make real
tiling terminals possible: `userland/sh.c`, a small userland shell, and
`tile` (`userland/rust/tile/`), a fixed-grid multiplexer that runs
multiple independent `sh` instances side by side.

```
AnssOS:/> run sh                 # a standalone userland shell
AnssOS:/> tile                   # two sh panes, side by side (default)
AnssOS:/> tile 4                 # a 2x2 grid of four
```

## Why this needed pipes first

Unlike `scarf`'s reverted window splits (`docs/scarf.md`) -- one process
rendering multiple *views of itself* -- this is genuinely independent
*processes* tiled together, which needs a way to redirect a child's
stdin/stdout away from the physical console. That didn't exist at all
before M19: `sys_write_impl`/`sys_read_impl` hardcoded fd 0/1/2 straight
to the keyboard/screen, bypassing the per-task file table entirely. See
[syscalls.md](syscalls.md#pipes-m19) for the `pipe()`/`use_as_stdio()`
primitives this needed, and why they're deliberately non-blocking —
that constraint (ring-0 syscall handlers can't be preempted, so a
busy-spin waiting on another process would deadlock) shaped the whole
design.

There was also no userland shell to `exec()` into a pane at all —
`kernel/src/shell/shell.c` is kernel-resident. `sh` ports a deliberately
narrow subset of it.

## sh

`cd`, `pwd`, `ls`, `cat`, `write <file> <text>`, `create dir|file
<name>`, `echo`, `clear`, `help`, and running any program (bare name
searches cwd then `/bin`, exactly `shell.c`'s own `resolve_program()`
logic, rewritten against syscalls). **Not implemented**: `delete`,
`copy`, `move`, `sync` (need `unlink`/`rename`/an explicit sync
syscall — none exist), and every kernel-debug builtin (`meminfo`,
`lspci`, `uptime`, `uname`, `crash`, `reboot`, `halt` — kernel-internal
introspection with no userland path). The kernel shell remains the tool
for real file management; `sh` is built for running programs and light
navigation inside a pane.

Its `read_line()` reads one byte at a time in a loop that treats a `0`
return as "no data yet, yield and try again" — this works identically whether
fd 0 is the physical console (which never actually returns 0 in raw
mode, it blocks internally instead) or a pipe (which does, per
[syscalls.md](syscalls.md#pipes-m19)'s non-blocking design) — one code
path, not two.

## tile

Rewritten in Rust with ratatui (`userland/rust/tile/`, M25). Each pane
has its own terminal emulator, so anything that runs on the console
runs in a pane too, including `scarf` and `play`.

```
 tile                                               pane 1 of 4     <- solid header
 # pane 1 ####################### - pane 2 -----------------------  <- focused pane's title solid,
 sh:/> ls                        # sh:/>                               others a rule
   bin/                          #
 sh:/> _                         #                                  <- solid divider
 - pane 3 ----------------------- - pane 4 -----------------------
 ...                             # ...
 ^B 1-4  focus   ^B o  next   ^B ^B  send ^B   ^B q  quit           <- key hints
```

| Key | Action |
|---|---|
| `Ctrl-b` then a digit | focus that pane |
| `Ctrl-b o` | focus the next pane |
| `Ctrl-b Ctrl-b` | send a literal `Ctrl-b` to the pane (scarf's sidebar toggle) |
| `Ctrl-b q` | quit: closes every pane's stdin and waits for them to exit |
| anything else | goes to the focused pane |

`tile` takes 1-4 panes. Two are side by side; three put the third across
the full bottom row; four make a 2x2 grid. A pane whose shell exits shows
`[exited]`, and tile exits once every pane has.

### How a pane works

1. **Spawning** (`pane.rs`). tile calls `pipe()` twice and `fork()`s.
   The child closes the ends it doesn't need, plus every earlier pane's
   fds (see "No close-on-exec" below). It points its stdin/stdout at the
   pipes with `use_as_stdio()`, sets its **window size to the pane's**
   with `TIOCSWINSZ`, and `exec`s `/bin/sh`.
2. **Window size.** `TIOCSWINSZ` stores the size on the process, and it
   is inherited by everything `sh` runs (see
   [syscalls.md](syscalls.md#terminal-handling)). So `scarf` or `play`
   started in a pane asks `TIOCGWINSZ` how big the terminal is and gets
   the pane's size, and lays itself out inside it.
3. **Output** (`vt.rs`). Everything a pane's programs write is fed
   through a terminal emulator that understands exactly the console's
   ANSI subset: cursor positioning, erase, reverse video, and immediate
   wrap with scroll at the bottom. Each pane is a grid of cells. A
   program's escape codes can therefore only ever affect its own pane;
   `clear` in a pane clears that pane.
4. **Drawing** (`ui.rs`). All the panes' grids, the titles, the dividers
   and the key hints are composed into one ratatui frame. Ratatui sends
   only the cells that changed since the last frame. The focused pane
   shows its cursor as a solid cell, unless the program hid it (as
   full-screen programs do).
5. **Input.** tile polls the keyboard and writes each key into the
   focused pane's stdin pipe. Programs that read keys with `read()`
   (`sh`, scarf) get them from that pipe. So does `play`, which uses
   `poll_key()`: the kernel reads a piped stdin instead of the keyboard
   for it, so a program in a pane never steals keys meant for tile.
6. **Quitting.** `Ctrl-b q` closes each pane's stdin. `sh` sees
   end-of-input and exits. A program running in a pane quits first:
   scarf on a closed `read()`, `play` because `poll_key()` returns -2 for
   a closed pipe. tile reaps each pane as its output pipe closes.

### Sharing the CPU

Nothing here blocks: pipes never do, and keys are polled. A loop that
just spun on an empty pipe would burn its whole time slice and starve
everything else, including `play`'s audio decoding when it runs in a
pane. So every such loop calls `sched_yield()` when it finds nothing to
do: tile's main loop, `sh`'s line reader, the `anssos` runtime's
`read_key()` and `write_all()` (a full pipe), and `play` while paused.
Tested with `play` in one pane and scarf in another: the captured audio
matches the reference decode with no dropouts.

### What was wrong before

The C version (`userland/tile.c`, M21) kept only scrollback text per
pane and copied pane output to the console byte for byte. That caused
three problems:

- **Escape codes leaked.** A pane printing an escape sequence, even
  `sh`'s `clear`, sent it to the real console, which wiped the whole
  screen.
- **The screen scrolled on every redraw.** A pane in the right column
  filled its last row up to the bottom-right corner of the screen. The
  console wraps as soon as a glyph lands in the last column, so writing
  that cell scrolled everything up a line each time: the header and the
  other panes drifted off the top. The anssos-tui backend never writes
  that cell.
- **Full-screen programs couldn't run in a pane.** They'd have drawn
  across the whole screen at its real size, and `play`'s `poll_key()`
  would have stolen tile's keys. `sh` refused to start `scarf` and
  `play` when run as a pane. That guard is gone now that both work.

## Two real bugs, both found by testing, not review

**Pipe refcounting.** The first version tracked "is this end open" as a
plain boolean. `fork()` shallow-copies the *entire* `open_files[]`
table (same as every other fd), so after spawning a pane both `tile`
and the new child hold independent table entries referencing the same
pipe — one process closing its own copy incorrectly closed the *shared*
object's end out from under the other. Caught by `userland/pipetest.c`'s
exec()'d-child case silently capturing zero bytes: the child's own
`close()` of its unneeded read-end copy closed the pipe's read end
before the exec'd program ever got a chance to write to it. Fixed by
making `read_refs`/`write_refs` real reference counts, with
`process_fork()` bumping them for every pipe-backed entry it copies.

**No close-on-exec.** Spawning a *second* pane's child inherits (via
the same full-table `fork()` copy) the *first* pane's `in_w`/`out_r`
too — real Unix has this exact problem, which is what `O_CLOEXEC`
exists to solve, and this project doesn't have it. The second child
never references the first pane's fds by name, so it never closes them,
silently holding a phantom write reference that keeps the first pipe's
refcount from ever reaching zero. Found as a genuine hang: `Ctrl-b q`
closed pane 1's stdin and waited forever in `waitpid()`, because pane
2's child was quietly still holding pane 1's write end open. Diagnosed
with temporary `kprintf` tracing in `pipe.c`/`syscall.c` (kernel debug
output goes to serial regardless of what's piped where, which is why it
was the right tool here rather than more guessing) that showed
`write_refs` at 2, not the expected 1, right before the close that
should have zeroed it. Fixed in `spawn_pane()`: each new pane's child
explicitly closes every *earlier* pane's `in_w`/`out_r` before
`use_as_stdio()`. The Rust version keeps the same fix (`Pane::spawn()`'s
`others`).

## Verification

Boot-tested incrementally, same discipline as M17/M18: `pipetest.bin`
(a `forktest.c`-shaped self-test) proved the basic round trip, the
non-blocking contract, and the real "capture an exec()'d child's
output" pattern *before* `sh`/`tile` were built on top of it; `sh` was
driven interactively and compared against the kernel shell for every
command it supports; `tile` was driven with two panes, confirming each
maintains independent state, keystrokes route only to the focused pane,
and `Ctrl-b q` reaps both children and returns cleanly to the launching
shell.
