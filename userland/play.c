/* An interactive WAV/MP3 CLI player -- plays a playlist of files given
 * on argv through the virtio-sound driver (kernel/src/drivers/virtio/
 * virtio_snd.c) via the audio_open()/audio_write()/audio_close()
 * syscalls, with poll_key() (a non-blocking keypress check -- see
 * kernel/src/exec/syscall.c) driving playback controls without ever
 * blocking the loop that feeds the device.
 *
 * WAV files must be PCM, 16-bit signed, mono/stereo, 44100/48000 Hz --
 * exactly what virtio_snd_open() accepts. MP3s (MPEG-1/2 layer I-III)
 * are decoded with minimp3 (third_party/minimp3, compiled in mp3.c) and
 * must also come out at 44100/48000 Hz. Which one a file is gets
 * sniffed from its first bytes, not its extension. minimp3 is
 * float-based, which works only because the kernel saves/restores each
 * process's x87/SSE state (kernel/src/arch/x86_64/fpu.h); volume and
 * the spectrum stay integer-only, as they predate that.
 *
 * The status display below reuses userland/scarf.c's exact ANSI/CSI
 * conventions (the only other AnssOS program that draws a fixed screen
 * rather than scrolling lines past) rather than inventing new ones: a
 * small abuf/ab_str/ab_int output buffer flushed with one write() per
 * redraw, every line positioned explicitly with ESC[row;colH instead of
 * \r\n (see ab_goto's comment), ESC[K to clear a line, ESC[7m/ESC[0m
 * for reverse video (the only "color" this console's ANSI parser
 * supports), and plain ASCII throughout -- font8x8_basic has no
 * box-drawing glyphs. */

#include "libc.h"
#include "third_party/minimp3/minimp3.h"

#include <stdint.h>

#define CHUNK_BYTES 4096

static int screencols;

/* ---------- small helpers (no realloc/snprintf in this libc) ---------- */

static void *grow(void *old, int oldn, int newn) {
    char *fresh = malloc((size_t)newn);
    if (fresh == NULL) {
        return NULL;
    }
    if (old != NULL) {
        memcpy(fresh, old, (size_t)(oldn < newn ? oldn : newn));
        free(old);
    }
    return fresh;
}

struct abuf {
    char *b;
    int len;
    int cap;
};

static void ab_append(struct abuf *ab, const char *s, int len) {
    if (ab->len + len > ab->cap) {
        int newcap = (ab->cap == 0) ? 1024 : ab->cap * 2;
        while (newcap < ab->len + len) {
            newcap *= 2;
        }
        char *fresh = grow(ab->b, ab->len, newcap);
        if (fresh == NULL) {
            return; /* Out of memory: drop output rather than corrupt it. */
        }
        ab->b = fresh;
        ab->cap = newcap;
    }
    memcpy(ab->b + ab->len, s, (size_t)len);
    ab->len += len;
}

static void ab_str(struct abuf *ab, const char *s) {
    ab_append(ab, s, (int)strlen(s));
}

static void ab_int(struct abuf *ab, int v) {
    char tmp[16];
    int i = 0;
    if (v < 0) {
        ab_str(ab, "-");
        v = -v;
    }
    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        ab_append(ab, &tmp[--i], 1);
    }
}

/* Zero-padded 2-digit int, for mm:ss clocks -- ab_int alone has no width
 * support (this libc's printf doesn't either, see stdio.c). */
static void ab_int2(struct abuf *ab, int v) {
    if (v < 0) {
        v = 0;
    }
    if (v > 99) {
        v = 99;
    }
    if (v < 10) {
        ab_str(ab, "0");
    }
    ab_int(ab, v);
}

static void ab_repeat(struct abuf *ab, char c, int n) {
    for (int i = 0; i < n; i++) {
        ab_append(ab, &c, 1);
    }
}

/* Places the cursor at a 1-based row/column -- every line is positioned
 * explicitly rather than separated by \r\n, same reasoning scarf.c's own
 * ab_goto gives: a line that exactly fills the width auto-wraps, and a
 * trailing newline then advances a *second* time, drifting the frame. */
static void ab_goto(struct abuf *ab, int row, int col) {
    ab_str(ab, "\x1b[");
    ab_int(ab, row);
    ab_str(ab, ";");
    ab_int(ab, col);
    ab_str(ab, "H");
}

/* ---------- spectrum analyzer (fixed-point Goertzel, no FPU needed) ---------- */

/* A cava-style per-frequency-band level meter needs a DFT, but this
 * kernel builds with -mno-sse/-mno-80387 everywhere (see the file
 * comment) -- no floating point compiles, let alone survives a
 * preemption. The Goertzel algorithm computes one DFT bin's magnitude as
 * a simple 2nd-order IIR recurrence with a single per-bin coefficient
 * (`2*cos(2*pi*f/fs)`), so unlike a full FFT it needs no sine/cosine
 * *at runtime* -- only that one constant per bar, which depends purely
 * on the target frequency and sample rate, not on how many samples get
 * fed through it. That constant is precomputed on the host (see the
 * table comment below) as a Q15 fixed-point value and hardcoded here;
 * the recurrence itself is plain int64_t arithmetic. */

#define N_BARS 20
#define BAR_ROWS 8
#define MAX_MONO_FRAMES (CHUNK_BYTES / 2) /* worst case: mono, 2 bytes/frame */

/* 20 log-spaced target frequencies from 60 Hz to 7000 Hz (the
 * perceptually useful range for a music visualizer), coeff[i] =
 * round(2*cos(2*pi*freq[i]/fs) * 32768) -- generated by a one-off host
 * script, not computed at build time (no libm/float here to compute
 * cos() with anyway). freqs (Hz): 60 77 99 127 163 210 270 346 445 572
 * 735 944 1212 1557 2001 2570 3302 4242 5449 7000. */
static const int32_t goertzel_coeff_44100[N_BARS] = {
    65534, 65532, 65529, 65525, 65518, 65507, 65488, 65456, 65404, 65319,
    65177, 64945, 64561, 63929, 62892, 61191, 58418, 53929, 46759, 35556,
};
static const int32_t goertzel_coeff_48000[N_BARS] = {
    65534, 65533, 65530, 65527, 65521, 65511, 65495, 65469, 65425, 65353,
    65233, 65037, 64713, 64179, 63302, 61862, 59510, 55692, 49560, 39896,
};

static int32_t mono_buf[MAX_MONO_FRAMES];

static int bitlen64(uint64_t v) {
    int n = 0;
    while (v != 0) {
        v >>= 1;
        n++;
    }
    return n;
}

/* Updates `bars` (N_BARS heights, 0..BAR_ROWS, persisted across calls)
 * in place from one chunk of S16LE PCM. The floor/divisor below were
 * calibrated by simulating this exact fixed-point recurrence in Python
 * against real audio (a pure 440Hz tone, and an actual music file) --
 * silence and quiet passages land under bit-length ~25, a present tone
 * or music bar lands roughly 27-47, so `(level - 25) / 3` spreads that
 * range across BAR_ROWS=8 for both. The one-row-per-call decay (rather
 * than snapping straight to the new height) is what keeps bars from
 * flickering between chunks -- a cheap "gravity" like a real level
 * meter, not a physically modeled one. */
static void compute_spectrum(const unsigned char *pcm, unsigned int bytes, unsigned int channels,
                              unsigned int rate, int *bars) {
    const short *samples = (const short *)pcm;
    unsigned int frames = bytes / (channels * 2u);
    if (frames > MAX_MONO_FRAMES) {
        frames = MAX_MONO_FRAMES;
    }

    for (unsigned int i = 0; i < frames; i++) {
        if (channels == 2) {
            mono_buf[i] = ((int32_t)samples[2 * i] + (int32_t)samples[2 * i + 1]) / 2;
        } else {
            mono_buf[i] = samples[i];
        }
    }

    const int32_t *coeffs = (rate == 48000) ? goertzel_coeff_48000 : goertzel_coeff_44100;

    for (int b = 0; b < N_BARS; b++) {
        int64_t coeff = coeffs[b];
        int64_t s1 = 0, s2 = 0;
        for (unsigned int i = 0; i < frames; i++) {
            int64_t s = (int64_t)mono_buf[i] + ((coeff * s1) >> 15) - s2;
            s2 = s1;
            s1 = s;
        }
        int64_t power = s1 * s1 + s2 * s2 - ((coeff * s1) >> 15) * s2;
        if (power < 0) {
            power = 0; /* fixed-point rounding can nudge this slightly negative near silence */
        }

        int height = (bitlen64((uint64_t)power) - 25) / 3;
        if (height < 0) {
            height = 0;
        }
        if (height > BAR_ROWS) {
            height = BAR_ROWS;
        }

        if (height > bars[b]) {
            bars[b] = height;
        } else if (bars[b] > 0) {
            bars[b]--;
        }
    }
}

static void get_window_size(void) {
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        screencols = ws.ws_col;
    } else {
        screencols = 80;
    }
    if (screencols < 40) {
        screencols = 40;
    }
}

/* ---------- the fixed player screen ---------- */

struct player_state {
    const char *path;
    int track_idx, track_count;
    unsigned int rate, channels;
    int mp3_kbps; /* 0 for WAV; an MP3's first audio frame's bitrate otherwise */
    unsigned int elapsed_sec, total_sec;
    int volume;
    int paused;
    int bars[N_BARS]; /* spectrum bar heights, 0..BAR_ROWS -- see compute_spectrum() */
};

#define ROW_HEADER 1
#define ROW_TRACK 3
#define ROW_FORMAT 4
#define ROW_SPECTRUM_TOP 6 /* BAR_ROWS rows tall: ROW_SPECTRUM_TOP..ROW_SPECTRUM_TOP+BAR_ROWS-1 */
#define ROW_PROGRESS (ROW_SPECTRUM_TOP + BAR_ROWS + 1)
#define ROW_VOLUME (ROW_PROGRESS + 2)
#define ROW_STATE (ROW_VOLUME + 1)
#define ROW_LEGEND (ROW_STATE + 2)

/* Shared by both the spectrum and the progress bar, so they always line
 * up at exactly the same width -- computed once here rather than in two
 * separate places that could drift apart. */
static int bar_display_width(void) {
    int w = screencols - 22;
    if (w < 10) {
        w = 10;
    }
    if (w > 60) {
        w = 60;
    }
    return w;
}

static void draw_screen(const struct player_state *st) {
    struct abuf ab = {NULL, 0, 0};
    ab_str(&ab, "\x1b[?25l"); /* we don't need the real cursor visible here */

    /* Header bar: reverse video, title left, "[i/N]" right. */
    struct abuf right = {NULL, 0, 0};
    ab_str(&right, "[");
    ab_int(&right, st->track_idx);
    ab_str(&right, "/");
    ab_int(&right, st->track_count);
    ab_str(&right, "] ");

    ab_goto(&ab, ROW_HEADER, 1);
    ab_str(&ab, "\x1b[7m");
    const char *title = " AnssOS play";
    ab_str(&ab, title);
    int pad = screencols - (int)strlen(title) - right.len;
    if (pad < 1) {
        pad = 1;
    }
    ab_repeat(&ab, ' ', pad);
    ab_append(&ab, right.b, right.len);
    ab_str(&ab, "\x1b[0m\x1b[K");
    free(right.b);

    ab_goto(&ab, ROW_TRACK, 1);
    ab_str(&ab, "Track:  ");
    ab_str(&ab, st->path);
    ab_str(&ab, "\x1b[K");

    ab_goto(&ab, ROW_FORMAT, 1);
    ab_str(&ab, "Format: ");
    ab_int(&ab, (int)st->rate);
    ab_str(&ab, " Hz, ");
    ab_str(&ab, st->channels == 1 ? "mono" : "stereo");
    if (st->mp3_kbps > 0) {
        ab_str(&ab, ", MP3 ");
        ab_int(&ab, st->mp3_kbps);
        ab_str(&ab, " kbps");
    } else {
        ab_str(&ab, ", 16-bit PCM");
    }
    ab_str(&ab, "\x1b[K");

    int bar_w = bar_display_width();

    /* Spectrum: BAR_ROWS rows x bar_w columns, same width as the
     * progress bar right below it. There are only N_BARS=20 actual
     * frequency bands (see compute_spectrum()), so each display column
     * samples the nearest band -- stretched wide on a roomy terminal,
     * compressed if bar_w ever ends up smaller than N_BARS. Plain ASCII,
     * no half-height glyphs (font8x8_basic has none), so BAR_ROWS is the
     * real vertical resolution, not a smoothing illusion. */
    for (int row = 0; row < BAR_ROWS; row++) {
        int level = BAR_ROWS - row;
        ab_goto(&ab, ROW_SPECTRUM_TOP + row, 1);
        for (int col = 0; col < bar_w; col++) {
            int bar_idx = col * N_BARS / bar_w;
            char c = (st->bars[bar_idx] >= level) ? '#' : ' ';
            ab_append(&ab, &c, 1);
        }
        ab_str(&ab, "\x1b[K");
    }

    /* Progress bar: "[####------] 00:12 / 01:30". */
    int filled = st->total_sec ? (int)(((long)st->elapsed_sec * bar_w) / st->total_sec) : 0;
    if (filled > bar_w) {
        filled = bar_w;
    }
    ab_goto(&ab, ROW_PROGRESS, 1);
    ab_str(&ab, "[");
    ab_repeat(&ab, '#', filled);
    ab_repeat(&ab, '-', bar_w - filled);
    ab_str(&ab, "] ");
    ab_int2(&ab, (int)(st->elapsed_sec / 60));
    ab_str(&ab, ":");
    ab_int2(&ab, (int)(st->elapsed_sec % 60));
    ab_str(&ab, " / ");
    ab_int2(&ab, (int)(st->total_sec / 60));
    ab_str(&ab, ":");
    ab_int2(&ab, (int)(st->total_sec % 60));
    ab_str(&ab, "\x1b[K");

    /* Volume bar: 0-200%, so 100% sits at the halfway mark. */
    int vol_w = 20;
    int vfilled = st->volume * vol_w / 200;
    if (vfilled < 0) {
        vfilled = 0;
    }
    if (vfilled > vol_w) {
        vfilled = vol_w;
    }
    ab_goto(&ab, ROW_VOLUME, 1);
    ab_str(&ab, "Volume: [");
    ab_repeat(&ab, '#', vfilled);
    ab_repeat(&ab, '-', vol_w - vfilled);
    ab_str(&ab, "] ");
    ab_int(&ab, st->volume);
    ab_str(&ab, "%");
    ab_str(&ab, "\x1b[K");

    ab_goto(&ab, ROW_STATE, 1);
    ab_str(&ab, "State:  ");
    ab_str(&ab, st->paused ? "PAUSED" : "PLAYING");
    ab_str(&ab, "\x1b[K");

    ab_goto(&ab, ROW_LEGEND, 1);
    ab_str(&ab, "space pause   n next   q quit   +/- volume");
    ab_str(&ab, "\x1b[K");

    write(1, ab.b, ab.len);
    free(ab.b);
}

/* ---------- track sources (WAV parsing, MP3 decoding) ---------- */

/* One open track, whichever format it is -- play_track() only ever
 * pulls 16-bit PCM out of it via src_read(). */
struct track_src {
    int fd;
    int is_mp3;
    unsigned int rate;
    unsigned int channels;
    unsigned int total_sec;
    int mp3_kbps;
    unsigned int remaining; /* WAV only: PCM bytes left in the `data` chunk */
};

static unsigned char chunk_buf[CHUNK_BYTES];

/* Per-track error messages, written out only after main()'s final
 * screen clear -- printing them directly would have them wiped by that
 * clear (or by the next track's redraw) before anyone could read them,
 * leaving a bad file looking like it "played" and finished instantly. */
static struct abuf errs = {NULL, 0, 0};

static int read_u32le(int fd, unsigned int *out) {
    unsigned char b[4];
    if (read(fd, b, 4) != 4) {
        return -1;
    }
    *out = (unsigned int)b[0] | ((unsigned int)b[1] << 8) | ((unsigned int)b[2] << 16) |
           ((unsigned int)b[3] << 24);
    return 0;
}

static int read_u16le(int fd, unsigned short *out) {
    unsigned char b[2];
    if (read(fd, b, 2) != 2) {
        return -1;
    }
    *out = (unsigned short)(b[0] | (b[1] << 8));
    return 0;
}

/* Walks RIFF chunks looking for `fmt `/`data` rather than assuming a
 * fixed layout -- some WAV files carry a LIST/fact chunk in between.
 * `fd` is positioned at the start of the file; on success it's left
 * open, positioned at the start of the PCM data. Closes it on failure. */
static int parse_wav(int fd, const char *path, struct track_src *out) {
    char magic[4];
    unsigned int riff_size;
    if (read(fd, magic, 4) != 4 || memcmp(magic, "RIFF", 4) != 0) {
        goto bad;
    }
    if (read_u32le(fd, &riff_size) != 0) {
        goto bad;
    }
    if (read(fd, magic, 4) != 4 || memcmp(magic, "WAVE", 4) != 0) {
        goto bad;
    }

    unsigned short audio_format = 0, channels = 0, bits_per_sample = 0, block_align = 0;
    unsigned int sample_rate = 0, byte_rate = 0;
    int have_fmt = 0;

    for (;;) {
        char chunk_id[4];
        unsigned int chunk_size;
        long n = read(fd, chunk_id, 4);
        if (n == 0) {
            break; /* EOF before `data` -- malformed */
        }
        if (n != 4 || read_u32le(fd, &chunk_size) != 0) {
            goto bad;
        }

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            if (read_u16le(fd, &audio_format) != 0 || read_u16le(fd, &channels) != 0 ||
                read_u32le(fd, &sample_rate) != 0 || read_u32le(fd, &byte_rate) != 0 ||
                read_u16le(fd, &block_align) != 0 || read_u16le(fd, &bits_per_sample) != 0) {
                goto bad;
            }
            have_fmt = 1;
            if (chunk_size > 16) {
                lseek(fd, chunk_size - 16, SEEK_CUR);
            }
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            if (!have_fmt) {
                goto bad;
            }
            if (audio_format != 1 || bits_per_sample != 16 ||
                (channels != 1 && channels != 2) ||
                (sample_rate != 44100 && sample_rate != 48000)) {
                ab_str(&errs, "play: ");
                ab_str(&errs, path);
                ab_str(&errs, ": unsupported format (fmt=");
                ab_int(&errs, audio_format);
                ab_str(&errs, " bits=");
                ab_int(&errs, bits_per_sample);
                ab_str(&errs, " ch=");
                ab_int(&errs, channels);
                ab_str(&errs, " rate=");
                ab_int(&errs, (int)sample_rate);
                ab_str(&errs, ") -- need PCM/16-bit/mono-or-stereo/44100-or-48000\n");
                close(fd);
                return -1;
            }
            /* Some WAV writers never patch the real length back into the
             * RIFF/data chunk-size fields once they're done streaming --
             * a declared size of 0xFFFFFFFF ("unknown") is the classic
             * case, but any size that overruns the actual file is the
             * same problem. Clamp to what's really left in the file
             * rather than trusting the header, or total_sec/the progress
             * bar would show a nonsensical multi-hour "duration" (the
             * header's own claim, taken at face value). Playback itself
             * would still have stopped correctly at EOF either way --
             * read()'s short-read/0 check already handles that -- this
             * only fixes what gets *displayed*. */
            long data_start = lseek(fd, 0, SEEK_CUR);
            long file_end = lseek(fd, 0, SEEK_END);
            lseek(fd, data_start, SEEK_SET);
            if (file_end > data_start) {
                unsigned int real_remaining = (unsigned int)(file_end - data_start);
                if (chunk_size > real_remaining) {
                    chunk_size = real_remaining;
                }
            }

            out->fd = fd;
            out->is_mp3 = 0;
            out->rate = sample_rate;
            out->channels = channels;
            out->total_sec = chunk_size / (sample_rate * channels * 2);
            out->mp3_kbps = 0;
            out->remaining = chunk_size;
            return 0;
        } else {
            lseek(fd, chunk_size, SEEK_CUR);
        }
        if (chunk_size & 1) {
            lseek(fd, 1, SEEK_CUR); /* chunks are word-aligned */
        }
    }

bad:
    ab_str(&errs, "play: ");
    ab_str(&errs, path);
    ab_str(&errs, ": not a valid WAV file\n");
    close(fd);
    return -1;
}

/* MP3 input is streamed through a fixed buffer -- the whole file can't
 * be read up front (a 4 MiB brk heap cap, see kernel/src/exec/
 * syscall.c), and doesn't need to be. Refilled whenever it drops below
 * half full, so minimp3 always sees well over the several consecutive
 * frames it wants before trusting a sync word. One track plays at a
 * time, so all of this is plain static state, reset by open_mp3(). */
#define MP3_IN_BYTES (16 * 1024)

static mp3dec_t mp3d;
static unsigned char mp3_in[MP3_IN_BYTES];
static int mp3_in_pos, mp3_in_len, mp3_eof;
static const unsigned char *mp3_last_frame; /* the frame decode_mp3_frame() last decoded */
static int mp3_last_frame_len;
static mp3d_sample_t mp3_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static unsigned int mp3_pcm_pos, mp3_pcm_len; /* bytes of mp3_pcm[] decoded / handed out */

static int looks_like_mp3(const unsigned char *b) {
    /* An ID3v2 tag, or a bare MPEG frame sync (11 set bits). */
    return memcmp(b, "ID3", 3) == 0 || (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0);
}

static unsigned int be32(const unsigned char *b) {
    return ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) | ((unsigned int)b[2] << 8) |
           (unsigned int)b[3];
}

/* Size of a leading ID3v2 tag (header, body, optional footer), 0 if
 * none -- skipped outright rather than left for minimp3 to scan past,
 * since embedded cover art can be hundreds of KiB. */
static long id3v2_size(int fd) {
    unsigned char h[10];
    lseek(fd, 0, SEEK_SET);
    if (read(fd, h, 10) != 10 || memcmp(h, "ID3", 3) != 0) {
        return 0;
    }
    long body = ((long)(h[6] & 0x7F) << 21) | ((long)(h[7] & 0x7F) << 14) |
                ((long)(h[8] & 0x7F) << 7) | (long)(h[9] & 0x7F); /* "synchsafe" */
    return 10 + body + ((h[5] & 0x10) ? 10 : 0);
}

/* Decodes the next audio frame into mp3_pcm[], returning its sample
 * count per channel -- 0 at end of stream. Frames minimp3 can't decode
 * (junk between frames, a trailing ID3v1 tag) are skipped. */
static int decode_mp3_frame(int fd, mp3dec_frame_info_t *info) {
    for (;;) {
        if (!mp3_eof && mp3_in_len - mp3_in_pos < MP3_IN_BYTES / 2) {
            memmove(mp3_in, mp3_in + mp3_in_pos, (size_t)(mp3_in_len - mp3_in_pos));
            mp3_in_len -= mp3_in_pos;
            mp3_in_pos = 0;
            while (mp3_in_len < MP3_IN_BYTES) {
                long n = read(fd, mp3_in + mp3_in_len, (size_t)(MP3_IN_BYTES - mp3_in_len));
                if (n <= 0) {
                    mp3_eof = 1;
                    break;
                }
                mp3_in_len += (int)n;
            }
        }

        int avail = mp3_in_len - mp3_in_pos;
        if (avail <= 0) {
            return 0;
        }
        int samples = mp3dec_decode_frame(&mp3d, mp3_in + mp3_in_pos, avail, mp3_pcm, info);
        if (info->frame_bytes == 0) {
            /* No complete frame anywhere in what's buffered (at least half
             * the buffer, unless at EOF) -- it's junk. Drop it. */
            if (mp3_eof) {
                return 0;
            }
            mp3_in_pos = mp3_in_len;
            continue;
        }
        mp3_last_frame = mp3_in + mp3_in_pos + info->frame_offset;
        mp3_last_frame_len = info->frame_bytes - info->frame_offset;
        mp3_in_pos += info->frame_bytes;
        if (samples > 0) {
            return samples;
        }
    }
}

/* A VBR file's first frame is usually a silent Xing/Info header frame
 * carrying the real frame count -- the only way to get an accurate
 * duration without scanning the whole file. It sits right after the
 * side info, whose size varies, so just look for the tag near the
 * start of the frame. Returns the frame count, or 0 if there isn't one. */
static unsigned int xing_frame_count(const unsigned char *frame, int len) {
    for (int off = 4; off + 12 <= len && off < 48; off++) {
        if (memcmp(frame + off, "Xing", 4) == 0 || memcmp(frame + off, "Info", 4) == 0) {
            unsigned int flags = be32(frame + off + 4);
            return (flags & 1) ? be32(frame + off + 8) : 0;
        }
    }
    return 0;
}

/* Sets up streaming decode of the MP3 on `fd` and decodes its first
 * frame (left pending in mp3_pcm[] for the first src_read()) -- the
 * output rate/channel count aren't known until then. Closes `fd` on
 * failure. */
static int open_mp3(int fd, const char *path, struct track_src *out) {
    long audio_start = id3v2_size(fd);
    long file_end = lseek(fd, 0, SEEK_END);
    lseek(fd, audio_start, SEEK_SET);

    mp3dec_init(&mp3d);
    mp3_in_pos = mp3_in_len = mp3_eof = 0;
    mp3_pcm_pos = mp3_pcm_len = 0;

    mp3dec_frame_info_t info;
    int samples = decode_mp3_frame(fd, &info);
    if (samples == 0) {
        ab_str(&errs, "play: ");
        ab_str(&errs, path);
        ab_str(&errs, ": no decodable MP3 audio found\n");
        close(fd);
        return -1;
    }
    if (info.hz != 44100 && info.hz != 48000) {
        ab_str(&errs, "play: ");
        ab_str(&errs, path);
        ab_str(&errs, ": unsupported MP3 sample rate ");
        ab_int(&errs, info.hz);
        ab_str(&errs, " Hz -- need 44100 or 48000\n");
        close(fd);
        return -1;
    }

    out->fd = fd;
    out->is_mp3 = 1;
    out->rate = (unsigned int)info.hz;
    out->channels = (unsigned int)info.channels;
    out->mp3_kbps = info.bitrate_kbps;
    out->remaining = 0;

    unsigned int xing_frames = xing_frame_count(mp3_last_frame, mp3_last_frame_len);
    if (xing_frames > 0) {
        out->total_sec = (unsigned int)((unsigned long)xing_frames * (unsigned long)samples /
                                        (unsigned long)info.hz);
    } else if (info.bitrate_kbps > 0 && file_end > audio_start) {
        /* No Xing header: assume CBR, so duration is just size/bitrate. */
        out->total_sec = (unsigned int)((file_end - audio_start) / (info.bitrate_kbps * 125L));
    } else {
        out->total_sec = 0;
    }

    mp3_pcm_len = (unsigned int)(samples * info.channels) * 2u;
    return 0;
}

/* Opens `path` and sniffs whether it's a WAV or an MP3. */
static int open_track(const char *path, struct track_src *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ab_str(&errs, "play: cannot open ");
        ab_str(&errs, path);
        ab_str(&errs, "\n");
        return -1;
    }
    unsigned char magic[4];
    if (read(fd, magic, 4) == 4) {
        lseek(fd, 0, SEEK_SET);
        if (memcmp(magic, "RIFF", 4) == 0) {
            return parse_wav(fd, path, out);
        }
        if (looks_like_mp3(magic)) {
            return open_mp3(fd, path, out);
        }
    }
    ab_str(&errs, "play: ");
    ab_str(&errs, path);
    ab_str(&errs, ": not a WAV or MP3 file\n");
    close(fd);
    return -1;
}

/* Fills `buf` with up to `max` bytes (a multiple of the frame size) of
 * 16-bit PCM from `src`. Returns the byte count, 0 at end of track. */
static long src_read(struct track_src *src, unsigned char *buf, unsigned int max) {
    if (!src->is_mp3) {
        unsigned int frame_bytes = src->channels * 2;
        unsigned int want = src->remaining < max ? src->remaining : max;
        want -= want % frame_bytes;
        if (want == 0) {
            return 0;
        }
        long n = read(src->fd, buf, want);
        if (n <= 0) {
            return 0;
        }
        src->remaining -= (unsigned int)n;
        return n;
    }

    while (mp3_pcm_pos == mp3_pcm_len) {
        mp3dec_frame_info_t info;
        int samples = decode_mp3_frame(src->fd, &info);
        if (samples == 0) {
            return 0;
        }
        /* A mid-stream rate/channel change (rare, but legal MP3) can't
         * be followed without reopening the audio stream -- skip any
         * such frame rather than play it at the wrong speed. */
        if ((unsigned int)info.hz != src->rate || (unsigned int)info.channels != src->channels) {
            continue;
        }
        mp3_pcm_pos = 0;
        mp3_pcm_len = (unsigned int)(samples * info.channels) * 2u;
    }
    unsigned int n = mp3_pcm_len - mp3_pcm_pos;
    if (n > max) {
        n = max;
    }
    memcpy(buf, (unsigned char *)mp3_pcm + mp3_pcm_pos, n);
    mp3_pcm_pos += n;
    return (long)n;
}

static short clamp16(int v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (short)v;
}

static void apply_volume(unsigned char *buf, unsigned int bytes, int volume) {
    if (volume == 100) {
        return;
    }
    short *samples = (short *)buf;
    unsigned int n = bytes / 2;
    for (unsigned int i = 0; i < n; i++) {
        samples[i] = clamp16(((int)samples[i] * volume) / 100);
    }
}

/* Returns 1 if the user quit ('q'), 0 on normal end-of-track/'n' skip,
 * -1 if the file couldn't be opened or parsed. */
static int play_track(const char *path, int track_idx, int track_count, int *volume) {
    struct track_src w;
    if (open_track(path, &w) != 0) {
        return -1;
    }

    if (audio_open(w.rate, w.channels) != 0) {
        ab_str(&errs, "play: audio_open failed (rate=");
        ab_int(&errs, (int)w.rate);
        ab_str(&errs, " channels=");
        ab_int(&errs, (int)w.channels);
        ab_str(&errs, ")\n");
        close(w.fd);
        return -1;
    }

    unsigned int bytes_per_sec = w.rate * w.channels * 2;
    unsigned int played_bytes = 0;
    int paused = 0;
    int quit = 0;
    int skip = 0;

    struct player_state st = {
        .path = path,
        .track_idx = track_idx,
        .track_count = track_count,
        .rate = w.rate,
        .channels = w.channels,
        .mp3_kbps = w.mp3_kbps,
        .elapsed_sec = 0,
        .total_sec = w.total_sec,
        .volume = *volume,
        .paused = 0,
    };
    draw_screen(&st);

    for (;;) {
        int dirty = 0;
        for (;;) {
            int key = poll_key();
            if (key == ' ') {
                paused = !paused;
                dirty = 1;
            } else if (key == 'q') {
                quit = 1;
                break;
            } else if (key == 'n') {
                skip = 1;
                break;
            } else if (key == '+') {
                *volume += 10;
                if (*volume > 200) {
                    *volume = 200;
                }
                dirty = 1;
            } else if (key == '-') {
                *volume -= 10;
                if (*volume < 0) {
                    *volume = 0;
                }
                dirty = 1;
            }
            if (dirty) {
                st.volume = *volume;
                st.paused = paused;
                draw_screen(&st);
            }
            if (!paused) {
                break;
            }
        }
        if (quit || skip) {
            break;
        }

        long n = src_read(&w, chunk_buf, CHUNK_BYTES);
        if (n <= 0) {
            break;
        }

        apply_volume(chunk_buf, (unsigned int)n, *volume);
        compute_spectrum(chunk_buf, (unsigned int)n, w.channels, w.rate, st.bars);

        if (audio_write(chunk_buf, (unsigned int)n) < 0) {
            break;
        }

        played_bytes += (unsigned int)n;

        /* Redrawn every chunk now, not just once/sec -- the spectrum
         * needs to actually move with the music. (elapsed_sec is folded
         * in here too, rather than gated separately, since there's no
         * point tracking it apart from the redraw it used to gate.) */
        st.elapsed_sec = bytes_per_sec ? played_bytes / bytes_per_sec : 0;
        draw_screen(&st);
    }

    audio_close();
    close(w.fd);
    return quit ? 1 : 0;
}

static int raw_mode_on(struct termios *orig) {
    if (tcgetattr(0, orig) != 0) {
        return -1;
    }
    struct termios raw = *orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(0, TCSANOW, &raw);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: play <file.wav|file.mp3> [more files ...]\n");
        exit(1);
    }

    struct termios orig;
    if (raw_mode_on(&orig) != 0) {
        printf("play: cannot put the terminal in raw mode\n");
        exit(1);
    }
    get_window_size();
    write(1, "\x1b[2J", 4);

    int volume = 100;
    int track_count = argc - 1;
    for (int i = 1; i < argc; i++) {
        int r = play_track(argv[i], i, track_count, &volume);
        if (r == 1) {
            break; /* user pressed q */
        }
    }

    write(1, "\x1b[0m\x1b[2J\x1b[H\x1b[?25h", 18);
    tcsetattr(0, TCSANOW, &orig);
    if (errs.len > 0) {
        write(1, errs.b, errs.len);
    }
    printf("play: done\n");
    exit(errs.len > 0 ? 1 : 0);
}
