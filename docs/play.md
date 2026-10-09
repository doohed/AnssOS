# play

An interactive WAV/MP3 player with a [ratatui](https://ratatui.rs) UI,
written in Rust (`userland/rust/play/`). It plays a playlist of files over the
`virtio-sound` driver (`kernel/src/drivers/virtio/virtio_snd.c`, M17).

```
> play testtone.wav                # the built-in test fixture
> play song1.mp3 song2.wav song3.mp3
```

## Screen

```
 AnssOS play                                              track 2 of 3    <- solid header bar

       song1.mp3

       MP3 320 kbps   48 kHz   stereo

       ----     ----                                                      <- peak markers
       ####     ####  ----
       #### #### #### #### ----      ----                                 <- solid bars
       #### #### #### #### #### #### ####  ####
       60 Hz         210           735          2k          7 kHz

       01:02  ##############-------------------------------------  03:45

       > PLAYING                              VOL ##########---------- 100%

 SPACE  pause   N  next   Q  quit   +/-  volume                           <- keycaps reversed
```

(`#` stands for a solid cell here.) The console's font (`font8x8_basic`)
is ASCII-only, and this screen is drawn in monochrome: reverse video
(`ESC[7m`) only, no colors. (The console has since gained the 16 ANSI
colors, see `docs/architecture.md`'s console section; `play` predates
them.) But a
reverse-video *space* is a solid cell, so the header and footer bars, the
spectrum's bars and the gauges' fill are all reversed spaces. A `#` in an
8×8 font reads as a cross-hatched grid instead.

The content is a column capped at 100 columns, centred horizontally and
vertically between the header and footer. The spectrum gets at most one
row per level (20), so the screen doesn't stretch edge to edge on a large
framebuffer. The spectrum's 20 bars always span exactly the column's
width, lined up with the progress and status rows below; columns that
don't divide evenly are spread across the gaps. Text lines are separated
by a blank row, because the 8×8 font has no leading and adjacent lines
touch. Everything adapts down to small terminals: the spectrum shrinks
first (minimum 3 rows), and labels that wouldn't fit are dropped.

## Keys

| Key | Action |
|---|---|
| `space` | pause / resume |
| `q` | stop and quit |
| `n` | skip to the next track |
| `+` / `-` | volume up/down by 10%, clamped 0-200% |

Controls only work over a **real terminal** — the same caveat
`userland/termtest.c`'s own comment already gives for raw mode: a
scripted, non-interactive input stream never produces the actual
keystrokes `poll_key()` is polling for.

## Format support

PCM, 16-bit signed little-endian, mono or stereo, 44100 Hz or 48000 Hz
only. That is not an arbitrary restriction — it is exactly what
`virtio_snd_open()` accepts (see `docs/architecture.md`); anything else
prints an error naming the actual unsupported field (format/bit depth/
channel count/rate) and moves on to the next track. The WAV parser
(`src/source.rs`) walks RIFF chunks looking for `fmt `/`data` rather than assuming
a fixed layout, since some WAV files carry a `LIST`/`fact` chunk in
between.

## MP3

`play` also plays MP3s (MPEG-1/2 layer I-III), decoded by
[minimp3](https://github.com/lieff/minimp3) -- a CC0 single-header
decoder vendored unmodified in `userland/rust/play/vendor/minimp3/` and
compiled in `userland/rust/play/c/mp3.c`, called from Rust over FFI
(`src/source.rs` mirrors its `mp3dec_t` struct field for field). The
format is sniffed from the file's first
bytes (`RIFF` vs. an ID3v2 tag or MPEG frame sync), never the extension,
so an MP3 named `.wav` plays too. The decoded rate still has to be
44100 or 48000 Hz; a mono or stereo MP3 at any bitrate is fine.

The file is streamed through a 16 KiB input buffer (a whole song won't
fit under the 4 MiB brk cap), a leading ID3v2 tag is skipped outright
(cover art can be hundreds of KiB), and the decoded frames feed the
exact same volume/spectrum/`audio_write()` path WAV playback uses. The
duration comes from a Xing/Info header when there is one (VBR files),
otherwise from file size / bitrate (assumes CBR).

This needed two kernel changes first, since minimp3 is float-based:

- **Per-process FPU/SSE state** (`kernel/src/arch/x86_64/fpu.c`).
  `fpu_init()` enables SSE (CR0/CR4), and `dispatch()` in
  `arch/x86_64/usermode.c` eagerly `FXSAVE`s/`FXRSTOR`s each process's
  registers around every dispatch -- including the nested one `wait()`
  does. The kernel itself still builds with `-mno-sse -mno-80387`, so it
  never touches those registers; userland now builds with SSE on.
- **A 64 KiB user stack** (was 16 KiB): `mp3dec_decode_frame()` alone
  keeps a ~16 KiB scratch struct on the stack.

Volume and the spectrum are integer-only (see below for why that matters
on the Rust side).

## Design notes

**No Rust std.** AnssOS has no Rust standard library, so Rust userland
is `#![no_std]` and lives in one Cargo workspace, `userland/rust/`, with
one lockfile and one `target/`:

```
userland/rust/
  Cargo.toml          workspace: members, shared deps, release profile
  .cargo/config.toml  target = x86_64-unknown-none
  anssos/             runtime crate shared by every Rust program
  tui/                anssos-tui: the ratatui console backend + setup/teardown
  play/
    src/              the player (lib.rs, source.rs, spectrum.rs, ui.rs)
    c/mp3.c           play's own C: compiles minimp3's implementation
    vendor/minimp3/   minimp3, vendored unmodified (CC0)
```

Each program is a **static library** for `x86_64-unknown-none`.
`scripts/build-userland.sh` builds the workspace with `cargo build
--release --manifest-path ... --config ...`, which works from any
directory, then links `libplay.a` with `crt0.o`, the hand-written C libc
and `c/mp3.c`, using the same `link.ld` as every C program. `crt0` calls
the `main` that `play` exports with `#[unsafe(no_mangle)] extern "C"`,
just as it would a C program's. The C parts are compiled by the build
script, not by Cargo, so they get exactly the same `CC`/`CFLAGS` as the
rest of userland.

The `anssos` runtime crate holds everything that isn't specific to
`play`:

- FFI bindings to the libc (`read`/`open`/`lseek`/`ioctl`/termios/the
  audio syscalls/`poll_key`), wrapped in RAII types (`File`, `Audio` and
  `RawMode` close or restore on drop).
- The global allocator, on the libc's `malloc()`/`free()`. That `malloc`
  only promises its own alignment, so every block is over-allocated and
  the original pointer is stashed just below the aligned one.
- The panic handler, which resets console attributes, shows the cursor,
  prints the panic and exits with code 101.

A new Rust program is a new workspace member that depends on `anssos`,
plus one `build_program` line in `build-userland.sh`.

**Ratatui without std.** Ratatui 0.30 supports `no_std`
(`default-features = false`; it needs `alloc`). Its built-in backends
(crossterm and the rest) need std, so `tui/src/backend.rs` (the
`anssos-tui` crate, shared with [scarf](scarf.md)) implements
`ratatui::backend::Backend` for the AnssOS console. It queues output into
one buffer and writes it with a single `write()` per frame. It emits only
the escapes the console understands: CUP, ED/EL, and SGR 7/0. CUP is
skipped for consecutive cells on the same row. Any non-ASCII symbol is
mapped to an ASCII stand-in, which is only a fallback.

The screen (`play/src/ui.rs`) draws through ratatui's `Frame`/`Buffer`,
`Line` and `Span`. The spectrum and the meters are small custom
widgets, because the stock `BarChart`/`Gauge` assume block glyphs or
a color palette `play` doesn't use. Geometry is plain `Rect` arithmetic
rather than ratatui's constraint-solver `Layout`: the crate is
soft-float, and the layout depends only on the terminal size.

**The bottom row stays empty.** The console wraps the cursor as soon as
a glyph lands in the last column; it has no deferred wrap. Writing the
bottom-right cell would therefore scroll the whole screen up a line, so
the backend reports one row fewer than the console has.

**Soft-float.** `x86_64-unknown-none` is a soft-float target, so the
little float math ratatui does runs in software. `play` avoids ratatui's
float-based layout solver entirely (see above). The hot paths
(PCM copy, volume and the Goertzel filters) stay integer. minimp3 is C
compiled with SSE and does use the FPU, which is safe because the kernel
saves and restores each process's FPU/SSE state.

**Redraw rate.** The screen redraws after every 4 KiB audio chunk, about
21 ms of 48 kHz stereo. Ratatui diffs each frame against the previous one
and only sends changed cells. Tested under TCG on an Apple Silicon host,
playback keeps up in real time. The guest's output, captured with QEMU's
`wav` audiodev, correlates 0.9999 with the host's own decode of the same
MP3, with no dropouts.

## Spectrum analyzer

A cava-style per-band level meter (`src/spectrum.rs`) needs some kind of
DFT, but the crate is soft-float, so a per-sample float FFT would be
expensive. The **Goertzel algorithm** avoids that. It computes a single
DFT bin's magnitude as a plain 2nd-order IIR recurrence,
```
s[n] = x[n] + coeff*s[n-1] - s[n-2]
power = s[n-1]^2 + s[n-2]^2 - coeff*s[n-1]*s[n-2]
```
with exactly one constant (`coeff = 2*cos(2*pi*f/fs)`) per frequency
band. That constant depends only on the target frequency and the sample
rate, so it's precomputed on the host as a Q15 fixed-point integer. There
are two 20-entry tables (one per supported sample rate), log-spaced from
60 Hz to 7000 Hz. The recurrence runs in `i64`, which leaves plenty of
headroom for ~2048 samples of `i16` input. The accumulator is bounded,
not exponentially unstable, because Goertzel's poles sit exactly on the
unit circle.

Turning raw power into a bar height needs a log scale. The power's bit
length is an exact integer stand-in for log2, and each bit is ~3 dB:
`level = bitlen(power) - 25`, clamped to 0-20, which spans 60 dB. The
floor of 25 was calibrated by simulating this exact recurrence against
real audio: quiet bands land under bit length ~25, and a present tone or
music at about 27-47. Most music sits in the lower half of that range,
which is why the top is cut at 45. Bars jump straight up but fall at
most 3 levels per chunk, a cheap "gravity" that keeps them from
flickering. A peak marker per band holds the recent maximum for 15 chunks
(~0.3 s), then falls one level per chunk.

## Adding your own audio

Two ways to get a file onto the VFS, since AnssOS has no network and no
host-mountable filesystem:

**Small fixtures, baked into the kernel image.** What `testtone.wav`
does: drop the file at `kernel/src/exec/<name>.wav.bin`, add an
`.incbin` pair in `userland_blobs.S`/`.h`, and a `vfs_write_bytes()`
call in `main.c`. Fine for a small built-in demo asset; costs a kernel
rebuild every time, and the bytes end up resident twice at runtime (once
in the kernel image itself, once in the VFS heap copy `vfs_write_bytes`
makes) — a bad fit for anything beyond a few hundred KB.

**Anything bigger: `scripts/disk-put.py`, straight onto the disk.**
`blkfs` (`kernel/src/fs/blkfs.c`) is AnssOS's own on-disk format — not a
real filesystem, just a flat recursive dump of the VFS tree (a 512-byte
superblock, then each node as type/name/size/data) — simple enough to
speak directly from the host, so this script is a faithful
reimplementation of `blkfs.c`'s own `serialize_node()`/
`deserialize_node()`. It reads whatever's already on `AnssOS-disk.img`
(if anything), adds/replaces a file at VFS root, and writes it back,
growing the image file as needed:
```
scripts/disk-put.py AnssOS-disk.img yourfile.wav
```
No kernel rebuild, no doubled memory cost, and it survives reboots the
same way anything `sync`'d from inside the VM would. The constraints
from "Format support" and "MP3" above still apply — the script doesn't
validate or convert audio, it just moves bytes.

A quirk found using this on a real-world file: some WAV writers never
patch the true length back into the `RIFF`/`data` chunk-size fields
(`0xFFFFFFFF`, a "streamed, size unknown at write time" placeholder) —
the parser clamps the declared `data` chunk size to what's
actually left in the file rather than trusting the header, so the
progress bar shows the real duration instead of a nonsensical
multi-hour one.

## What it needed from the kernel

| Capability | Why |
|---|---|
| `drivers/virtio/virtio_snd.c` | no audio driver of any kind existed before M17 |
| `audio_open`/`audio_write`/`audio_close` syscalls | Linux does audio via `/dev/snd/*` + `ioctl`; AnssOS has no devfs, so there was no existing syscall shape to reuse |
| `poll_key` syscall | a playback loop has to check for a control key *without* blocking — `read()`'s raw-mode "wait for one byte" behavior would stall audio output; this is `shell.c`'s own non-blocking `virtio_input_poll_char()`/`serial_poll_char()` poll, exposed as a syscall |
| `testtone.wav` boot fixture (`scripts/gen-test-tone.py`) | there is no host-side way to get an arbitrary file onto the VFS (same reason `filetest.txt` is a boot-time fixture), so `play testtone.wav` needs something built in to play |

## Verifying it actually plays something

`scripts/run-qemu.sh` defaults `-audiodev` to QEMU's `wav` backend,
capturing whatever the guest sends to `AnssOS-audio-out.wav` on the
host (gitignored, like `AnssOS-disk.img`) — this works on any host with
no real audio hardware or PulseAudio/ALSA setup required, so `play` is
boot-verifiable the same deterministic way virtio-gpu is verified by
screendump. Inspect the captured file afterward (e.g. Python's `wave`
module) to confirm non-silent samples of roughly the right
duration/frequency came out. To actually *hear* it on a machine with
working audio, override the backend: `QEMU_AUDIODEV="pa,id=snd0"`
(PulseAudio), `"alsa,id=snd0"` (ALSA), or `"coreaudio,id=snd0"`
(macOS).

## Not supported

Seeking, a persistent playlist file, per-track metadata display beyond
the filename (ID3 tags are skipped, not read), and formats other than PCM
WAV and MP3. Sample rates other than 44100/48000 Hz would need
resampling.
