#include "fbconsole.h"
#include "font8x16.h"
#include "../lib/string.h"

#include <stdint.h>

#define GLYPH_W 8
#define GLYPH_H 16
/* Blank margin around the text area, in font pixels (times the scale). */
#define PAD 16
#define FG_COLOR 0x00FFFFFFu /* BGRX8888: white. */
#define BG_COLOR 0x00000000u /* BGRX8888: black. */

static struct framebuffer *fb;
/* Each font pixel is drawn as a scale x scale block, so a cell is
 * cell_w x cell_h screen pixels -- 8x16 glyphs at 1:1 are too small on a
 * 1080p or 4K monitor. Picked from the width in fbconsole_init(). The
 * grid starts pad_x/pad_y pixels in from the top-left corner, with at
 * least that much margin on the other two sides as well. */
static uint32_t scale, cell_w, cell_h;
static uint32_t pad_x, pad_y;
static uint32_t cols, rows;
/* The part of the screen drawn since the last flush, in pixels:
 * [dirty_x0, dirty_x1) x [dirty_y0, dirty_y1), empty when x0 >= x1. */
static uint32_t dirty_x0, dirty_y0, dirty_x1, dirty_y1;
static uint32_t cursor_col, cursor_row;
/* Current SGR attributes -- see handle_csi()'s 'm' case. fg/bg are
 * palette indexes, -1 for the defaults (FG_COLOR/BG_COLOR). */
static int reverse_video;
static int fg_index = -1, bg_index = -1;
static int bold, dim;

/* The 16 ANSI colors (SGR 30-37/90-97 foreground, 40-47/100-107
 * background), as 0x00RRGGBB -- which is what a BGRX8888 pixel is in a
 * little-endian uint32_t. Picked to read well on black at small sizes:
 * the dark eight are mid-bright rather than the dim classic VGA set. */
static const uint32_t PALETTE[16] = {
    0x00000000u, 0x00D0504Fu, 0x0060B050u, 0x00D8A840u, /* black red green yellow */
    0x004A80D8u, 0x00B060C8u, 0x0040A8B8u, 0x00C0C0C0u, /* blue magenta cyan white */
    0x005C5C5Cu, 0x00FF7070u, 0x0090E080u, 0x00FFD870u, /* bright: black(grey) red green yellow */
    0x0070B0FFu, 0x00E090FFu, 0x0070E0F0u, 0x00FFFFFFu, /* bright: blue magenta cyan white */
};

/* Halfway between two colors, per channel -- SGR 2 (dim) text. */
static uint32_t blend(uint32_t a, uint32_t b) {
    return ((a >> 1) & 0x007F7F7Fu) + ((b >> 1) & 0x007F7F7Fu);
}
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

static void mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (dirty_x0 >= dirty_x1) {
        dirty_x0 = x;
        dirty_y0 = y;
        dirty_x1 = x + w;
        dirty_y1 = y + h;
        return;
    }
    if (x < dirty_x0) {
        dirty_x0 = x;
    }
    if (y < dirty_y0) {
        dirty_y0 = y;
    }
    if (x + w > dirty_x1) {
        dirty_x1 = x + w;
    }
    if (y + h > dirty_y1) {
        dirty_y1 = y + h;
    }
}

/* Makes everything drawn since the last flush visible. */
static void flush(void) {
    if (dirty_x0 >= dirty_x1) {
        return;
    }
    display_flush_rect(dirty_x0, dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
    dirty_x0 = dirty_x1 = 0;
}

/* Fills one cell-sized-or-smaller rectangle; callers mark it dirty. */
static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t py = y; py < y + h; py++) {
        volatile uint32_t *row = fb->pixels + (uint64_t)py * fb->pitch;
        for (uint32_t px = x; px < x + w; px++) {
            row[px] = color;
        }
    }
}

/* UTF-8 decoding state: a multi-byte character arrives one byte per
 * fbconsole_putc() call. */
static uint32_t utf8_cp;
static int utf8_need; /* continuation bytes still expected */

/* The bitmap for a code point: ASCII and the extras (box drawing,
 * prompt-theme arrows and rounds, a few symbols) from font8x16.h, and
 * '?' for anything else. */
static const uint8_t *glyph_for(uint32_t cp) {
    if (cp < 128) {
        return font8x16_ascii[cp];
    }
    for (size_t i = 0; i < sizeof(font8x16_ext) / sizeof(font8x16_ext[0]); i++) {
        if (font8x16_ext[i].cp == cp) {
            return font8x16_ext[i].rows;
        }
    }
    return font8x16_ascii['?'];
}

static void draw_glyph(uint32_t col, uint32_t row, uint32_t cp) {
    const uint8_t *glyph = glyph_for(cp);
    uint32_t base_x = pad_x + col * cell_w;
    uint32_t base_y = pad_y + row * cell_h;
    /* Bold is drawn as the bright variant of a dark color: the bitmap
     * font has no bold weight. */
    int fg_i = (bold && fg_index >= 0 && fg_index < 8) ? fg_index + 8 : fg_index;
    uint32_t fg = fg_i >= 0 ? PALETTE[fg_i] : FG_COLOR;
    uint32_t bg = bg_index >= 0 ? PALETTE[bg_index] : BG_COLOR;
    if (dim) {
        fg = blend(fg, bg);
    }
    if (reverse_video) {
        uint32_t t = fg;
        fg = bg;
        bg = t;
    }

    for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
            fill_rect(base_x + gx * scale, base_y + gy * scale, scale, scale,
                      (bits & (1u << gx)) ? fg : bg);
        }
    }
    mark_dirty(base_x, base_y, cell_w, cell_h);
}

/* Erases `count` cells starting at (col, row), always to the background
 * colour -- deliberately not draw_glyph(' '), which would paint the
 * inverted block that reverse video turns a space into. */
static void erase_cells(uint32_t col, uint32_t row, uint32_t count) {
    if (col >= cols || count == 0) {
        return;
    }
    if (count > cols - col) {
        count = cols - col;
    }
    uint32_t x = pad_x + col * cell_w, y = pad_y + row * cell_h;
    fill_rect(x, y, count * cell_w, cell_h, BG_COLOR);
    mark_dirty(x, y, count * cell_w, cell_h);
}

/* Moves the text area up one row of cells. Only the rows * cell_h pixels
 * the grid covers take part -- the margins stay blank. */
static void scroll(void) {
    uint64_t row_pixels = (uint64_t)fb->pitch * cell_h;
    uint64_t text_pixels = row_pixels * rows;
    volatile uint32_t *top = fb->pixels + (uint64_t)fb->pitch * pad_y;
    memmove((void *)top, (void *)(top + row_pixels), (text_pixels - row_pixels) * sizeof(uint32_t));
    fill_rect(0, pad_y + (rows - 1) * cell_h, fb->width, cell_h, BG_COLOR);
    mark_dirty(0, pad_y, fb->width, rows * cell_h);
}

void fbconsole_init(struct framebuffer *the_fb) {
    fb = the_fb;
    scale = fb->width >= 3200 ? 3 : fb->width >= 1600 ? 2 : 1;
    cell_w = GLYPH_W * scale;
    cell_h = GLYPH_H * scale;
    pad_x = pad_y = PAD * scale;
    cols = (fb->width - 2 * pad_x) / cell_w;
    rows = (fb->height - 2 * pad_y) / cell_h;
    fbconsole_clear();
}

void fbconsole_clear(void) {
    if (fb == NULL) {
        return; /* No screen -- the kernel shell's `clear` still calls this. */
    }
    fill_rect(0, 0, fb->width, fb->height, BG_COLOR);
    mark_dirty(0, 0, fb->width, fb->height);
    cursor_col = 0;
    cursor_row = 0;
    reverse_video = 0;
    fg_index = -1;
    bg_index = -1;
    bold = 0;
    dim = 0;
    pstate = P_NORMAL;
}

void fbconsole_cell_size(uint32_t *out_w, uint32_t *out_h) {
    *out_w = cell_w;
    *out_h = cell_h;
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
        flush();
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
        case 'm': { /* SGR -- the 16 ANSI colors, bold, dim and reverse. */
            if (nparams == 0) {
                reverse_video = 0;
                fg_index = bg_index = -1;
                bold = dim = 0;
            }
            for (int i = 0; i < nparams; i++) {
                uint32_t p = params[i];
                if (p == 0) {
                    reverse_video = 0;
                    fg_index = bg_index = -1;
                    bold = dim = 0;
                } else if (p == 1) {
                    bold = 1;
                } else if (p == 2) {
                    dim = 1;
                } else if (p == 7) {
                    reverse_video = 1;
                } else if (p == 22) {
                    bold = dim = 0;
                } else if (p == 27) {
                    reverse_video = 0;
                } else if (p >= 30 && p <= 37) {
                    fg_index = (int)(p - 30);
                } else if (p == 39) {
                    fg_index = -1;
                } else if (p >= 40 && p <= 47) {
                    bg_index = (int)(p - 40);
                } else if (p == 49) {
                    bg_index = -1;
                } else if (p >= 90 && p <= 97) {
                    fg_index = (int)(p - 90 + 8);
                } else if (p >= 100 && p <= 107) {
                    bg_index = (int)(p - 100 + 8);
                }
                /* Anything else (underline, 256-color, ...) is ignored. */
            }
            break;
        }
        default:
            break; /* Includes ESC[?25l/h (cursor visibility) -- nothing to do
                    * here, since this console draws no cursor of its own. */
    }
}

/* Draws one character at the cursor and advances: wrapping immediately
 * after the last column (no deferred wrap), scrolling past the last row. */
static void put_glyph(uint32_t cp) {
    draw_glyph(cursor_col, cursor_row, cp);
    cursor_col++;
    if (cursor_col >= cols) {
        cursor_col = 0;
        cursor_row++;
    }
    if (cursor_row >= rows) {
        scroll();
        cursor_row = rows - 1;
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
    /* UTF-8: a lead byte starts a character, continuation bytes finish
     * it; a malformed sequence draws one '?' and is dropped. */
    uint8_t b = (uint8_t)c;
    if (utf8_need > 0) {
        if ((b & 0xC0) == 0x80) {
            utf8_cp = (utf8_cp << 6) | (b & 0x3F);
            if (--utf8_need == 0) {
                put_glyph(utf8_cp);
            }
            return;
        }
        utf8_need = 0;
        put_glyph('?'); /* truncated sequence; then handle `b` itself */
    }
    if (b >= 0x80) {
        if ((b & 0xE0) == 0xC0) {
            utf8_cp = b & 0x1F;
            utf8_need = 1;
        } else if ((b & 0xF0) == 0xE0) {
            utf8_cp = b & 0x0F;
            utf8_need = 2;
        } else if ((b & 0xF8) == 0xF0) {
            utf8_cp = b & 0x07;
            utf8_need = 3;
        } else {
            put_glyph('?'); /* a stray continuation byte */
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
        if (cursor_row >= rows) {
            scroll();
            cursor_row = rows - 1;
        }
        return;
    }
    put_glyph(b);
}

void fbconsole_draw_text_at(uint32_t col, uint32_t row, const char *s) {
    while (*s) {
        draw_glyph(col, row, (uint8_t)*s);
        col++;
        s++;
    }
}

void fbconsole_write(const char *s) {
    while (*s) {
        fbconsole_putc(*s++);
    }
    flush();
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
     * character during bulk log output (e.g. `help`'s ~20 lines) on
     * virtio-gpu, and a one-cell copy on a GOP framebuffer, which is
     * cheap enough in practice not to matter. */
    flush();
}
