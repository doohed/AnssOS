#include "fbconsole.h"
#include "font8x8_basic.h"
#include "../lib/string.h"

#include <stdint.h>

#define GLYPH_W 8
#define GLYPH_H 8
#define FG_COLOR 0x00FFFFFFu /* BGRX8888: white. */
#define BG_COLOR 0x00000000u /* BGRX8888: black. */

static struct virtio_gpu_fb *fb;
static uint32_t cols, rows;
static uint32_t cursor_col, cursor_row;
static int reverse_video; /* SGR 7 -- see handle_csi()'s 'm' case. */
static int batching, batch_dirty;

/* Escape-sequence parser state. Everything this console understands is a
 * CSI sequence (ESC '[' params final-byte); a bare ESC followed by
 * anything else is dropped rather than printed, since a half-understood
 * sequence painting literal junk on screen is worse than nothing. */
#define MAX_PARAMS 8
static enum { P_NORMAL, P_ESC, P_CSI } pstate;
static uint32_t params[MAX_PARAMS];
static int nparams;
static int csi_private; /* A '?' right after the '[' -- e.g. ESC[?25l. */

static void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    fb->pixels[y * fb->width + x] = color;
}

static void draw_glyph(uint32_t col, uint32_t row, char c) {
    uint8_t code = (uint8_t)c;
    if (code > 127) {
        code = '?';
    }
    const uint8_t *glyph = font8x8_basic[code];
    uint32_t base_x = col * GLYPH_W;
    uint32_t base_y = row * GLYPH_H;
    uint32_t fg = reverse_video ? BG_COLOR : FG_COLOR;
    uint32_t bg = reverse_video ? FG_COLOR : BG_COLOR;

    for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
            put_pixel(base_x + gx, base_y + gy, (bits & (1u << gx)) ? fg : bg);
        }
    }
}

/* Erases `count` cells starting at (col, row), always to the background
 * colour -- deliberately not draw_glyph(' '), which would paint the
 * inverted block that reverse video turns a space into. */
static void erase_cells(uint32_t col, uint32_t row, uint32_t count) {
    for (uint32_t i = 0; i < count && col + i < cols; i++) {
        uint32_t base_x = (col + i) * GLYPH_W;
        uint32_t base_y = row * GLYPH_H;
        for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
            for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
                put_pixel(base_x + gx, base_y + gy, BG_COLOR);
            }
        }
    }
}

static void scroll(void) {
    uint32_t row_pixels = fb->width * GLYPH_H;
    uint32_t total_pixels = fb->width * fb->height;
    memmove((void *)fb->pixels, (void *)(fb->pixels + row_pixels),
            (total_pixels - row_pixels) * sizeof(uint32_t));
    for (uint32_t i = total_pixels - row_pixels; i < total_pixels; i++) {
        fb->pixels[i] = BG_COLOR;
    }
}

void fbconsole_init(struct virtio_gpu_fb *the_fb) {
    fb = the_fb;
    cols = fb->width / GLYPH_W;
    rows = fb->height / GLYPH_H;
    fbconsole_clear();
}

void fbconsole_clear(void) {
    for (uint32_t i = 0; i < fb->width * fb->height; i++) {
        fb->pixels[i] = BG_COLOR;
    }
    cursor_col = 0;
    cursor_row = 0;
    reverse_video = 0;
    pstate = P_NORMAL;
}

int fbconsole_size(uint32_t *out_cols, uint32_t *out_rows) {
    if (fb == NULL) {
        return -1;
    }
    *out_cols = cols;
    *out_rows = rows;
    return 0;
}

void fbconsole_begin_batch(void) {
    batching = 1;
}

void fbconsole_end_batch(void) {
    batching = 0;
    if (fb != NULL && batch_dirty) {
        batch_dirty = 0;
        virtio_gpu_flush();
    }
}

/* param n, defaulting to `fallback` when it was omitted entirely (ESC[H
 * and ESC[1;1H mean the same thing) -- nparams only counts parameters
 * that were actually present. */
static uint32_t param_or(int n, uint32_t fallback) {
    if (n >= nparams || params[n] == 0) {
        return fallback;
    }
    return params[n];
}

/* Dispatches a complete CSI sequence on its final byte. Anything not
 * understood is swallowed silently -- a terminal that prints the raw
 * bytes of a sequence it doesn't implement is strictly worse than one
 * that ignores it. */
static void handle_csi(char final) {
    switch (final) {
        case 'H': /* CUP -- cursor position, 1-based row;col. */
        case 'f': {
            uint32_t row = param_or(0, 1) - 1;
            uint32_t col = param_or(1, 1) - 1;
            cursor_row = row < rows ? row : rows - 1;
            cursor_col = col < cols ? col : cols - 1;
            break;
        }
        case 'A': { /* CUU/CUD/CUF/CUB -- relative cursor moves, clamped. */
            uint32_t n = param_or(0, 1);
            cursor_row = n > cursor_row ? 0 : cursor_row - n;
            break;
        }
        case 'B': {
            uint32_t n = param_or(0, 1);
            cursor_row = cursor_row + n >= rows ? rows - 1 : cursor_row + n;
            break;
        }
        case 'C': {
            uint32_t n = param_or(0, 1);
            cursor_col = cursor_col + n >= cols ? cols - 1 : cursor_col + n;
            break;
        }
        case 'D': {
            uint32_t n = param_or(0, 1);
            cursor_col = n > cursor_col ? 0 : cursor_col - n;
            break;
        }
        case 'J': { /* ED -- erase display. Does not move the cursor. */
            uint32_t mode = nparams > 0 ? params[0] : 0;
            if (mode == 2) {
                for (uint32_t r = 0; r < rows; r++) {
                    erase_cells(0, r, cols);
                }
            } else if (mode == 1) {
                for (uint32_t r = 0; r < cursor_row; r++) {
                    erase_cells(0, r, cols);
                }
                erase_cells(0, cursor_row, cursor_col + 1);
            } else {
                erase_cells(cursor_col, cursor_row, cols - cursor_col);
                for (uint32_t r = cursor_row + 1; r < rows; r++) {
                    erase_cells(0, r, cols);
                }
            }
            break;
        }
        case 'K': { /* EL -- erase line. Does not move the cursor. */
            uint32_t mode = nparams > 0 ? params[0] : 0;
            if (mode == 2) {
                erase_cells(0, cursor_row, cols);
            } else if (mode == 1) {
                erase_cells(0, cursor_row, cursor_col + 1);
            } else {
                erase_cells(cursor_col, cursor_row, cols - cursor_col);
            }
            break;
        }
        case 'm': { /* SGR -- only reverse video, which is how userland/scarf.c
                     * draws its cursor (portable: real terminals do this too,
                     * so the serial path renders identically). No colour
                     * support; those parameters are ignored, not an error. */
            if (nparams == 0) {
                reverse_video = 0;
            }
            for (int i = 0; i < nparams; i++) {
                if (params[i] == 0 || params[i] == 27) {
                    reverse_video = 0;
                } else if (params[i] == 7) {
                    reverse_video = 1;
                }
            }
            break;
        }
        default:
            break; /* Includes ESC[?25l/h (cursor visibility) -- nothing to do
                    * here, since this console draws no cursor of its own. */
    }
}

void fbconsole_putc(char c) {
    if (pstate == P_ESC) {
        if (c == '[') {
            pstate = P_CSI;
            nparams = 0;
            csi_private = 0;
            for (int i = 0; i < MAX_PARAMS; i++) {
                params[i] = 0;
            }
        } else {
            pstate = P_NORMAL; /* Not a CSI -- drop it rather than print it. */
        }
        return;
    }
    if (pstate == P_CSI) {
        if (c == '?' && nparams == 0) {
            csi_private = 1;
        } else if (c >= '0' && c <= '9') {
            if (nparams == 0) {
                nparams = 1;
            }
            params[nparams - 1] = params[nparams - 1] * 10 + (uint32_t)(c - '0');
        } else if (c == ';') {
            /* An omitted parameter still occupies a slot (ESC[;5H means
             * "default row, column 5"), so a separator seen before any
             * digit has to claim slot 0 before opening the next one. */
            if (nparams == 0) {
                nparams = 1;
            }
            if (nparams < MAX_PARAMS) {
                nparams++;
            }
        } else {
            if (!csi_private) {
                handle_csi(c);
            }
            pstate = P_NORMAL;
        }
        return;
    }
    if (c == 0x1b) {
        pstate = P_ESC;
        return;
    }

    if (c == '\r') {
        cursor_col = 0;
        return;
    }
    if (c == '\b') {
        /* No-op at column 0 -- deliberately not walking back onto the
         * previous line, since we don't track where lines actually
         * ended (the caller wrapped mid-word or not). Good enough for
         * the shell's single-line input editing. */
        if (cursor_col > 0) {
            cursor_col--;
            draw_glyph(cursor_col, cursor_row, ' ');
        }
        return;
    }
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else {
        draw_glyph(cursor_col, cursor_row, c);
        cursor_col++;
        if (cursor_col >= cols) {
            cursor_col = 0;
            cursor_row++;
        }
    }

    if (cursor_row >= rows) {
        scroll();
        cursor_row = rows - 1;
    }
}

void fbconsole_draw_text_at(uint32_t col, uint32_t row, const char *s) {
    while (*s) {
        draw_glyph(col, row, *s);
        col++;
        s++;
    }
}

void fbconsole_write(const char *s) {
    while (*s) {
        fbconsole_putc(*s++);
    }
    virtio_gpu_flush();
}

void fbconsole_kprintf_sink(char c) {
    fbconsole_putc(c);
    if (batching) {
        batch_dirty = 1; /* fbconsole_end_batch() does the one flush. */
        return;
    }
    /* Flush every character, not just on '\n' -- this sink also carries
     * interactive shell input echo (drivers/serial.c's RX path / the
     * virtio-input keyboard), and a typed character that doesn't show up
     * on screen until Enter looks exactly like the keystroke never
     * arrived at all. Costs a couple of virtqueue round trips per
     * character during bulk log output (e.g. `help`'s ~20 lines), which
     * is cheap enough in practice not to matter. */
    virtio_gpu_flush();
}
