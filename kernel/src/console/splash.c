#include "splash.h"
#include "braille_art.h"
#include "fbconsole.h"
#include "../drivers/pit.h"
#include "../drivers/virtio/virtio_gpu.h"

#include <stdint.h>

#define GLYPH_W 8 /* Must match console/fbconsole.c's glyph size. */
#define GLYPH_H 8

#define ART_SCALE 3           /* Each braille dot becomes an ART_SCALE x ART_SCALE pixel block. */
#define ART_COLOR 0x00FFFFFFu /* BGRX8888: white. */
#define CAPTION "AnssOS"

#define ANIM_FRAME_MS 250 /* Real time now, via drivers/pit.c -- no more guessed spin count. */
#define ANIM_CYCLES 4

/* Fixed-width so each frame fully overwrites the last -- otherwise ".."
 * would leave a stray "." behind when it shrinks back down to "." */
static const char *const DOT_FRAMES[] = {
    ".  ",
    ".. ",
    "...",
};
#define DOT_FRAME_COUNT ((uint32_t)(sizeof(DOT_FRAMES) / sizeof(DOT_FRAMES[0])))

/* A Unicode Braille Pattern character (U+2800-U+28FF) packs an 8-dot,
 * 2-wide x 4-tall grid into one glyph. In UTF-8 that's always a 3-byte
 * sequence: 0xE2 followed by a second byte in 0xA0-0xA3 and a third byte
 * that's a normal UTF-8 continuation byte (0x80-0xBF). The dot mask is
 * the low 2 bits of the second byte combined with the low 6 bits of the
 * third. Anything else in the source (a stray plain-ASCII space, say) is
 * treated as a blank cell so a malformed line can't desync the decoder.
 */
static int decode_braille_cell(const char *s, int *consumed) {
    uint8_t b0 = (uint8_t)s[0];

    if (b0 == 0xE2) {
        uint8_t b1 = (uint8_t)s[1];
        uint8_t b2 = (uint8_t)s[2];
        *consumed = 3;
        if (b1 >= 0xA0 && b1 <= 0xA3) {
            return ((b1 & 0x03) << 6) | (b2 & 0x3F);
        }
        return 0; /* Some other 3-byte UTF-8 character -- draw as blank. */
    }
    if (b0 < 0x80) {
        *consumed = 1;
        return 0;
    }
    if ((b0 & 0xE0) == 0xC0) {
        *consumed = 2;
        return 0;
    }
    if ((b0 & 0xF0) == 0xF0) {
        *consumed = 4;
        return 0;
    }
    *consumed = 1;
    return 0;
}

/* Bit i of a braille dot mask -> its (column, row) position within the
 * glyph's 2x4 dot grid, per the Unicode Braille Patterns block layout. */
static const uint8_t DOT_DX[8] = {0, 0, 0, 1, 1, 1, 0, 1};
static const uint8_t DOT_DY[8] = {0, 1, 2, 0, 1, 2, 3, 3};

static uint32_t line_cell_count(const char *line) {
    uint32_t count = 0;
    while (*line) {
        int consumed;
        decode_braille_cell(line, &consumed);
        line += consumed;
        count++;
    }
    return count;
}

static void draw_pixel_block(struct virtio_gpu_fb *fb, uint32_t x, uint32_t y, uint32_t color) {
    for (uint32_t sy = 0; sy < ART_SCALE; sy++) {
        for (uint32_t sx = 0; sx < ART_SCALE; sx++) {
            uint32_t px = x + sx;
            uint32_t py = y + sy;
            if (px < fb->width && py < fb->height) {
                fb->pixels[py * fb->width + px] = color;
            }
        }
    }
}

/* Decodes and draws SPLASH_ART centered horizontally, starting at pixel
 * row start_y. Returns the pixel row just past the art's bottom edge, so
 * the caller can place a caption/animation under it. */
static uint32_t draw_art(struct virtio_gpu_fb *fb, uint32_t start_y) {
    uint32_t cell_cols = line_cell_count(SPLASH_ART[0]);
    uint32_t art_w = cell_cols * 2 * ART_SCALE;
    uint32_t start_x = (fb->width > art_w) ? (fb->width - art_w) / 2 : 0;

    for (uint32_t row = 0; row < SPLASH_ART_LINES; row++) {
        const char *p = SPLASH_ART[row];
        uint32_t col = 0;
        while (*p) {
            int consumed;
            int mask = decode_braille_cell(p, &consumed);
            p += consumed;

            for (int bit = 0; bit < 8; bit++) {
                if (!(mask & (1 << bit))) {
                    continue;
                }
                uint32_t px = start_x + (col * 2 + DOT_DX[bit]) * ART_SCALE;
                uint32_t py = start_y + (row * 4 + DOT_DY[bit]) * ART_SCALE;
                draw_pixel_block(fb, px, py, ART_COLOR);
            }
            col++;
        }
    }

    return start_y + SPLASH_ART_LINES * 4 * ART_SCALE;
}

static uint32_t text_width(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

void splash_show(struct virtio_gpu_fb *fb) {
    uint32_t cols = fb->width / GLYPH_W;

    uint32_t art_h = SPLASH_ART_LINES * 4 * ART_SCALE;
    uint32_t caption_h = GLYPH_H * 2; /* Caption line + a blank line under the art. */
    uint32_t total_h = art_h + caption_h;
    uint32_t start_y = (fb->height > total_h) ? (fb->height - total_h) / 2 : 0;

    uint32_t art_bottom_y = draw_art(fb, start_y);

    uint32_t caption_row = art_bottom_y / GLYPH_H + 1;
    uint32_t caption_col = (cols > text_width(CAPTION)) ? (cols - text_width(CAPTION)) / 2 : 0;
    fbconsole_draw_text_at(caption_col, caption_row, CAPTION);

    uint32_t dots_row = caption_row + 2;
    uint32_t dots_width = text_width(DOT_FRAMES[DOT_FRAME_COUNT - 1]);
    uint32_t dots_col = (cols > dots_width) ? (cols - dots_width) / 2 : 0;

    virtio_gpu_flush();

    for (uint32_t cycle = 0; cycle < ANIM_CYCLES; cycle++) {
        for (uint32_t f = 0; f < DOT_FRAME_COUNT; f++) {
            fbconsole_draw_text_at(dots_col, dots_row, DOT_FRAMES[f]);
            virtio_gpu_flush();
            pit_sleep_ms(ANIM_FRAME_MS);
        }
    }
}
