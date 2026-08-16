# play

An interactive WAV/PCM CLI player. It lives in `userland/play.c` and
plays a playlist of files over the new `virtio-sound` driver
(`kernel/src/drivers/virtio/virtio_snd.c`, M17).

```
AnssOS:/> play testtone.wav                # the built-in test fixture
AnssOS:/> play song1.wav song2.wav song3.wav
```

## Screen

```
 AnssOS play                                                          [1/3]
Track:  song1.wav
Format: 44100 Hz, stereo, 16-bit PCM

    #  #
   ## # #
   ######## # #
   ############  ####
   ##################

[##############################------------------------------] 01:02 / 02:00

Volume: [###########---------] 110%
State:  PLAYING

space pause   n next   q quit   +/- volume
```

The header bar is reverse video (`ESC[7m`) — the only "color" this
console's ANSI parser supports (no SGR color codes, see
`docs/architecture.md`'s console section). Everything else is plain
ASCII; `font8x8_basic` has no box-drawing glyphs to draw a real border
with. The spectrum sits between the format line and the duration bar,
and is deliberately drawn at the *exact same width* as the duration bar
below it (`bar_display_width()` computes it once, shared by both) rather
than each picking its own — see "Spectrum analyzer" below for how the
spectrum itself works without any floating point.

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
channel count/rate) and moves on to the next track. `play.c`'s own WAV
parser walks RIFF chunks looking for `fmt `/`data` rather than assuming
a fixed layout, since some WAV files carry a `LIST`/`fact` chunk in
between.

## Why WAV, not MP3

The user asked for an "mp3 player." Real MP3 decoding (Huffman-coded
bitstream, IMDCT, a 32-band polyphase synthesis filterbank) is a large,
float-heavy undertaking, and this kernel currently has **no FPU/SSE
context-switch support at all** — both `kernel/GNUmakefile` and
`scripts/build-userland.sh` build with `-mno-sse -mno-sse2 -mno-80387
-mno-mmx`, so floating point doesn't even compile today, let alone
survive a preemption safely. `play`'s volume control is deliberately
integer-only (`sample * volume / 100`) for exactly that reason. Real
MP3 support is future work, in order: (1) FPU/SSE context-switch
support (`XSAVE`/`FXSAVE` per-task, plus dropping `-mno-sse` for
userland), (2) a decoder (porting something small like `minimp3` rather
than hand-rolling Huffman/IMDCT/synthesis from scratch), (3) feeding its
PCM output through the exact same `audio_write()` path this milestone
already built.

## Design notes

The screen reuses `userland/scarf.c`'s exact conventions rather than
inventing new ones — it's the only other AnssOS program that draws a
fixed screen instead of scrolling lines: a small `abuf`/`ab_str`/
`ab_int` output buffer flushed with one `write()` per redraw, every line
positioned explicitly with `ESC[row;colH` instead of `\r\n` (a line that
exactly fills the terminal width auto-wraps, and a trailing newline
after it then advances a *second* time — the same drifting-frame bug
`scarf.md`'s design notes already describe), and `ESC[K` to clear a line
before redrawing it.

A full redraw happens on **every** 4096-byte audio chunk (~46ms) now,
not throttled to once/second the way an earlier version of this did —
the spectrum needs to actually move with the music. That seemed risky
given `scarf.md#performance`'s warning that a full-framebuffer
`virtio_gpu_flush()` "dominates" under TCG, so it was measured rather
than assumed: timing a full 2.000s clip end-to-end (host wall-clock,
`play` issuing one redraw per chunk throughout) came out to 2.02s real
time — no measurable lag. The flush cost scarf hit is apparently
dominated by TCG's virtqueue round-trip overhead itself, not by how
much of the framebuffer changed or whether a real display is attached,
so redrawing more of the same small screen doesn't cost meaningfully
more per redraw — only redrawing *more often* would, and once-per-chunk
turned out to be cheap enough.

## Spectrum analyzer

A cava-style per-frequency-band level meter needs some kind of DFT, but
a real FFT is exactly the float-heavy territory "Why WAV, not MP3" above
already ruled out for this kernel. The **Goertzel algorithm** sidesteps
that: it
computes a single DFT bin's magnitude as a plain 2nd-order IIR
recurrence,
```
s[n] = x[n] + coeff*s[n-1] - s[n-2]
power = s[n-1]^2 + s[n-2]^2 - coeff*s[n-1]*s[n-2]
```
with exactly one constant (`coeff = 2*cos(2*pi*f/fs)`) per frequency
band, and — critically — that constant depends only on the target
frequency and sample rate, not on the block size or anything computed
at runtime. So it can be precomputed *on the host* as a Q15 fixed-point
integer and hardcoded, needing no `cos()` (and thus no float) in AnssOS
itself at all: `userland/play.c` has two 20-entry tables (one per
supported sample rate), generated by a one-off Python calculation and
pasted in as `static const int32_t` literals, log-spaced from 60 Hz to
7000 Hz. The recurrence itself runs in `int64_t` (comfortable headroom
for ~2048 samples of `int16_t`-range input; the accumulator is bounded,
not exponentially unstable, since Goertzel's poles sit exactly on the
unit circle).

Only 20 actual frequency bands come out of this (`N_BARS`, one per
coefficient table entry) — matching that to whatever width the terminal
actually gives the bar would need recomputing coefficients at runtime,
which is exactly the "no `cos()` at runtime" constraint this whole
approach exists to avoid. Instead `draw_screen()` stretches (or, on a
narrow terminal, compresses) those 20 bands across `bar_display_width()`
display columns by nearest-neighbor lookup (`bar_idx = col * N_BARS /
bar_w`) — cheap, and the band boundaries are already coarse (log-spaced,
DFT-leakage-blurred) enough that a few adjacent columns sharing one
band's value doesn't read as an artifact.

Turning raw power into a 0-8 bar height needs a magnitude scale, which
normally means `sqrt`/`log` — also unavailable without float. Bit-length
of the (integer) power value is a free, exact stand-in for log2, so
`height = (bitlen64(power) - 25) / 3` — floor and divisor picked by
simulating this *exact* fixed-point recurrence in Python against real
audio first (a pure 440 Hz tone and an actual music file) rather than
guessing: silence/quiet bands land under bit-length ~25, a present tone
or music band lands ~27-47, and that range maps cleanly onto 8 rows.
Verified in-VM afterward two ways: the 440 Hz test tone produces exactly
one dominant bar at the band nearest 445 Hz (its neighbors show mild
spectral leakage, everything else near-silent) confirming the tables and
recurrence are correct; real music produces a visibly different bar
pattern between two snapshots ~1.6s apart, confirming it's actually
reactive and not stuck. Heights decay at most one row per redraw rather
than snapping straight to the new value — the "gravity" a real level
meter has, and cheap enough to be worth doing (one comparison per bar).

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
same way anything `sync`'d from inside the VM would. Only PCM
constraints from "Format support" above still apply — the script
doesn't validate or convert audio, it just moves bytes.

A quirk found using this on a real-world file: some WAV writers never
patch the true length back into the `RIFF`/`data` chunk-size fields
(`0xFFFFFFFF`, a "streamed, size unknown at write time" placeholder) —
`play.c`'s parser clamps the declared `data` chunk size to what's
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
the filename, and (see above) anything but PCM WAV.
