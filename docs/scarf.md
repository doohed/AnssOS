# scarf

A small modal text editor with vim keybindings and a file-explorer
sidebar. It lives in `userland/scarf.c` and is the first AnssOS program
that draws a full screen rather than scrolling log lines past.

```
AnssOS:/> scarf                  # sidebar on the current directory
AnssOS:/> scarf notes.txt        # open a file
AnssOS:/docs> scarf .            # sidebar on /docs
```

A directory argument opens the sidebar there; a file argument opens
straight into the editor. With no argument it starts in whatever
directory the shell was in, since a launched process inherits the
shell's cwd.

## Keys

### Sidebar

| Key | Action |
|---|---|
| `Ctrl-b` | show/hide the sidebar (the key VS Code uses) |
| `Ctrl-e` | move focus between sidebar and editor |
| `j` / `k` | move the selection |
| `Enter` / `l` | open a file, or descend into a directory |
| `h` / `-` | go up to the parent directory |
| `g` / `G` | jump to first/last entry |
| `r` | re-read the listing |
| `Esc` | back to the editor |

Directories are shown with a `>` marker and a trailing `/`. When you are
not at the root, `..` is always the first entry.

### Normal mode

| Key | Action |
|---|---|
| `h` `j` `k` `l` | move by character/line |
| `w` / `b` | forward/back a word |
| `0` / `$` | start/end of line |
| `gg` / `G` | first/last line |
| `i` | insert before the cursor |
| `a` / `A` | insert after the cursor / at end of line |
| `o` / `O` | open a line below/above |
| `x` | delete the character under the cursor |
| `dd` / `dw` | delete line / delete word |
| `:` | command line |

### Insert mode

`Esc` returns to normal mode, leaving the cursor on the last inserted
character the way vim does. `Enter` splits the line, `Backspace` at
column 0 joins with the previous line, and `Tab` inserts four spaces.

### Command line

| Command | Action |
|---|---|
| `:w` / `:w <path>` | save / save as |
| `:q` / `:q!` | quit / quit discarding changes |
| `:wq` / `:x` | save and quit |
| `:e <path>` / `:e! <path>` | open a file / open discarding changes |

## Design notes

**The cursor is drawn as a reverse-video cell** (SGR 7) rather than the
terminal's own cursor, because that renders identically on the
framebuffer console and over serial. The real cursor is hidden with
`ESC[?25l` on entry and restored on exit, along with normal video and a
cleared screen -- otherwise the shell prompt inherits reverse video and
an invisible cursor.

**Every line is positioned explicitly** with `ESC[row;colH` rather than
separated by `\r\n`. A line that exactly fills the terminal width
auto-wraps to the next row, and a trailing newline after it would then
advance a *second* time -- drifting the whole frame down one row per
redraw until the screen scrolls and leaves a stale duplicate behind.
That bug was found by screendumping the framebuffer and noticing the
message line rendered twice.

**Directory detection uses `chdir()`, not `opendir()`.** `opendir()` is
just `open(path, O_RDONLY)`, which succeeds on a regular file too, so it
cannot tell the two apart. `chdir()` rejects anything that is not a
directory, and canonicalises as a side effect -- which is why `scarf .`
shows `/docs` in the header rather than the literal `/docs/.`.

**Window splits were tried and removed.** Tiled panes cannot use `ESC[K`
to clear a line (it would erase the pane beside it), so every cell had
to be padded with spaces -- roughly 20 KB of output per keystroke. The
current layout is deliberately one editor pane reaching the right edge,
so it clears with `ESC[K` and writes only as many bytes as the text
actually occupies.

## Performance

Two things make redraws expensive, and only one of them is fixed:

**Byte count (addressed).** The editor pane clears with `ESC[K` instead
of padding. The sidebar still has to pad (the editor is to its right),
so it is redrawn *only when it changes* -- not while you are typing.
Measured around 2.4 KB per keystroke, against ~16 KB minimum for the
padded-everything approach.

**Framebuffer flush (not addressed).** `virtio_gpu_flush()` transfers
the *entire* framebuffer -- 4 MB at 1280x800 -- with two synchronous
virtqueue round trips, on every redraw, no matter how little changed.
Under TCG this dominates. The fix is a dirty-rectangle flush: track the
changed region in `fbconsole.c` and pass those bounds in `xfer_req.r` /
`flush_req.r` instead of `fb.width`/`fb.height`.

## What it needed from the kernel

scarf is the reason several kernel capabilities exist. All of them
landed alongside it:

| Capability | Why |
|---|---|
| `TIOCGWINSZ` | nothing could ask how big the terminal was |
| `O_TRUNC` | writes only ever *grew* a file, so saving a shortened buffer left the old tail behind |
| ANSI/CSI parser in `fbconsole.c` | cursor addressing had nowhere to land -- escapes were drawn as literal glyphs |
| Escape in the virtio keymap | no way to leave insert mode in a graphical window |
| Shifted symbol row in the keymap | `:` was unreachable, so `:w` and `:q` could not be typed |
| Ctrl tracking in the keymap | `Ctrl-b`/`Ctrl-e` are control bytes a serial terminal sends itself, but the keymap had no concept of Ctrl |
| `argv` | no way to pass a path to a program |
| cwd inheritance | `.` always resolved to `/`, whatever directory you launched from |
| `getcwd` | the sidebar header needs a canonical path to show |

## Not supported

Visual mode, registers/yank/put, undo, and search. Deliberately, to keep
the thing reviewable. The libc it is built on has no `realloc` and no
`snprintf`, so it carries small local substitutes for both.
