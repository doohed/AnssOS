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

    for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
            put_pixel(base_x + gx, base_y + gy, (bits & (1u << gx)) ? FG_COLOR : BG_COLOR);
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
}

void fbconsole_putc(char c) {
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
    if (c == '\n') {
        virtio_gpu_flush();
    }
}
