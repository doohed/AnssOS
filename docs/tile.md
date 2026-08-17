# sh and tile

Two new pieces (M19-M21) that together make real tiling terminals
possible: `userland/sh.c`, a small userland shell, and `userland/tile.c`,
a fixed-grid multiplexer that runs multiple independent `sh` instances
side by side.

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
return as "no data yet, keep looping" — this works identically whether
fd 0 is the physical console (which never actually returns 0 in raw
mode, it blocks internally instead) or a pipe (which does, per
[syscalls.md](syscalls.md#pipes-m19)'s non-blocking design) — one code
path, not two.

## tile

| Key | Action |
|---|---|
| `Ctrl-b` then a digit | switch focus to that pane |
| `Ctrl-b q` | quit (closes every pane's stdin, waits for clean exit) |
| anything else | goes to the focused pane |

A fixed grid (1/2/2x2 panes, not a dynamic resizable tree — the exact
feature `scarf` tried and reverted, sidestepped entirely by not having
it), each cell scrollback-only: `sh` never emits cursor-addressing
escapes, so there's no ANSI to interpret, just `\r`/`\n`/backspace
bookkeeping. **Running `scarf`/`play` as a pane is out of scope** — they
emit real ANSI (cursor addressing, `ESC[K`, reverse video) that this
virtual terminal doesn't parse; that would need a real per-pane ANSI
interpreter, i.e. reimplementing `fbconsole.c`'s parser once per pane.
This isn't just "renders oddly in its cell" either: `TIOCGWINSZ` has no
concept of panes and always reports the *physical* screen size, so a
full-screen program run inside a pane draws with absolute coordinates
across the whole screen, corrupting `tile`'s header and every other
pane too — and its own input loop, written assuming a blocking
console read, busy-loops redrawing at full speed the moment `read()`
starts returning `0` (empty pipe) instead. `sh` guards against this
directly rather than let it happen: `tile.c` passes a `--tile-pane`
flag when spawning each pane, and `sh` refuses to run a short list of
known full-screen programs (currently `scarf`, `play`) when that flag
is set, with a clear error instead of a corrupted screen. It's a guard
rail, not a fix — those programs still can't actually run in a pane,
they just fail cleanly now instead of breaking the display.

Content grows top-down within each cell, like a real terminal, not
bottom-pinned — a pane's cell is nearly full-screen tall (few panes,
one shared header row), so an earlier version that bottom-aligned a
few lines of prompt text looked like an empty box with text stuck at
the very bottom. `draw_pane()` places scrollback right after the label
row and blanks out whatever's left *below* it; once there's enough
scrollback to fill the whole cell, the current line naturally lands on
the last row with no blank rows left — real terminal-scrolling
behavior falls out of the same code path, not a separate case.

Spawning a pane is `pipe()` twice, `fork()`, the child closes the ends
it doesn't need and `use_as_stdio()`s the rest before `execve()`, the
parent keeps the other two — see [syscalls.md](syscalls.md#pipes-m19)
for the exact pattern. Redraws only happen for panes that actually
produced new output, not on a timer.

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
`use_as_stdio()`.

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
