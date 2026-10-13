//! Small scrolling text console drawn with an 8x16 bitmap font (font.rs)
//! over a framebuffer (drivers/display.rs), with a margin around the
//! text, both scaled up 2x or 3x on wide screens. Understands UTF-8 and
//! the handful of escape sequences full-screen programs use (cursor
//! moves, erase, the 16 ANSI colors). Flushes the part it changed after
//! every write.

use super::font;
use crate::drivers::display::{self, Framebuffer};
use crate::sync::Global;

const GLYPH_W: u32 = 8;
const GLYPH_H: u32 = 16;
/// Blank margin around the text area, in font pixels (times the scale).
const PAD: u32 = 16;
const FG_COLOR: u32 = 0x00FF_FFFF; // BGRX8888: white.
const BG_COLOR: u32 = 0x0000_0000; // BGRX8888: black.

/// The 16 ANSI colors (SGR 30-37/90-97 foreground, 40-47/100-107
/// background), as 0x00RRGGBB -- which is what a BGRX8888 pixel is in a
/// little-endian u32. Picked to read well on black at small sizes: the
/// dark eight are mid-bright rather than the dim classic VGA set.
#[rustfmt::skip]
const PALETTE: [u32; 16] = [
    0x0000_0000, 0x00D0_504F, 0x0060_B050, 0x00D8_A840, // black red green yellow
    0x004A_80D8, 0x00B0_60C8, 0x0040_A8B8, 0x00C0_C0C0, // blue magenta cyan white
    0x005C_5C5C, 0x00FF_7070, 0x0090_E080, 0x00FF_D870, // bright: black(grey) red green yellow
    0x0070_B0FF, 0x00E0_90FF, 0x0070_E0F0, 0x00FF_FFFF, // bright: blue magenta cyan white
];

/// Halfway between two colors, per channel -- SGR 2 (dim) text.
fn blend(a: u32, b: u32) -> u32 {
    ((a >> 1) & 0x007F_7F7F) + ((b >> 1) & 0x007F_7F7F)
}

const MAX_PARAMS: usize = 8;

/// Escape-sequence parser state. Everything this console understands is a
/// CSI sequence (ESC '[' params final-byte); a bare ESC followed by
/// anything else is dropped rather than printed, since a half-understood
/// sequence painting literal junk on screen is worse than nothing.
#[derive(Clone, Copy, PartialEq)]
enum Parse {
    Normal,
    Esc,
    Csi,
}

struct Console {
    fb: Option<Framebuffer>,
    /// Each font pixel is drawn as a scale x scale block, so a cell is
    /// cell_w x cell_h screen pixels -- 8x16 glyphs at 1:1 are too small on
    /// a 1080p or 4K monitor. Picked from the width in init(). The grid
    /// starts pad_x/pad_y pixels in from the top-left corner, with at least
    /// that much margin on the other two sides as well.
    scale: u32,
    cell_w: u32,
    cell_h: u32,
    pad_x: u32,
    pad_y: u32,
    cols: u32,
    rows: u32,
    /// The part of the screen drawn since the last flush, in pixels:
    /// [x0, x1) x [y0, y1), empty when x0 >= x1.
    dirty: (u32, u32, u32, u32),
    cursor_col: u32,
    cursor_row: u32,
    /// Current SGR attributes -- see handle_csi()'s 'm' case. fg/bg are
    /// palette indexes, None for the defaults (FG_COLOR/BG_COLOR).
    reverse_video: bool,
    fg: Option<usize>,
    bg: Option<usize>,
    bold: bool,
    dim: bool,
    batching: bool,
    batch_dirty: bool,
    parse: Parse,
    params: [u32; MAX_PARAMS],
    /// How many parameters were actually present (ESC[H has none).
    nparams: usize,
    /// A '?' right after the '[' -- e.g. ESC[?25l.
    csi_private: bool,
    /// UTF-8 decoding: a multi-byte character arrives one byte per putc().
    utf8_cp: u32,
    /// Continuation bytes still expected.
    utf8_need: u32,
}

static CONSOLE: Global<Console> = Global::new(Console {
    fb: None,
    scale: 1,
    cell_w: GLYPH_W,
    cell_h: GLYPH_H,
    pad_x: 0,
    pad_y: 0,
    cols: 0,
    rows: 0,
    dirty: (0, 0, 0, 0),
    cursor_col: 0,
    cursor_row: 0,
    reverse_video: false,
    fg: None,
    bg: None,
    bold: false,
    dim: false,
    batching: false,
    batch_dirty: false,
    parse: Parse::Normal,
    params: [0; MAX_PARAMS],
    nparams: 0,
    csi_private: false,
    utf8_cp: 0,
    utf8_need: 0,
});

fn console() -> &'static mut Console {
    unsafe { CONSOLE.get() }
}

/// The bitmap for a code point: ASCII and the extras (box drawing,
/// prompt-theme arrows and rounds, a few symbols) from font.rs, and '?'
/// for anything else.
fn glyph_for(cp: u32) -> &'static [u8; 16] {
    if cp < 128 {
        return &font::ASCII[cp as usize];
    }
    font::EXT.iter().find(|(c, _)| *c == cp).map_or(&font::ASCII[b'?' as usize], |(_, rows)| rows)
}

impl Console {
    fn mark_dirty(&mut self, x: u32, y: u32, w: u32, h: u32) {
        let (x0, y0, x1, y1) = self.dirty;
        self.dirty = if x0 >= x1 { (x, y, x + w, y + h) } else { (x0.min(x), y0.min(y), x1.max(x + w), y1.max(y + h)) };
    }

    /// Makes everything drawn since the last flush visible.
    fn flush(&mut self) {
        let (x0, y0, x1, y1) = self.dirty;
        if x0 >= x1 {
            return;
        }
        display::flush_rect(x0, y0, x1 - x0, y1 - y0);
        self.dirty = (0, 0, 0, 0);
    }

    /// Fills a rectangle; callers mark it dirty.
    fn fill_rect(&self, x: u32, y: u32, w: u32, h: u32, color: u32) {
        let Some(fb) = self.fb else { return };
        for py in y..y + h {
            let row =
                unsafe { core::slice::from_raw_parts_mut(fb.pixels.add((py * fb.pitch + x) as usize), w as usize) };
            row.fill(color);
        }
    }

    fn draw_glyph(&mut self, col: u32, row: u32, cp: u32) {
        let glyph = glyph_for(cp);
        let (base_x, base_y) = (self.pad_x + col * self.cell_w, self.pad_y + row * self.cell_h);
        // Bold is drawn as the bright variant of a dark color: the bitmap
        // font has no bold weight.
        let fg_i = match self.fg {
            Some(i) if self.bold && i < 8 => Some(i + 8),
            other => other,
        };
        let mut fg = fg_i.map_or(FG_COLOR, |i| PALETTE[i]);
        let mut bg = self.bg.map_or(BG_COLOR, |i| PALETTE[i]);
        if self.dim {
            fg = blend(fg, bg);
        }
        if self.reverse_video {
            core::mem::swap(&mut fg, &mut bg);
        }
        let s = self.scale;
        for (gy, bits) in glyph.iter().enumerate() {
            for gx in 0..GLYPH_W {
                let color = if bits & (1 << gx) != 0 { fg } else { bg };
                self.fill_rect(base_x + gx * s, base_y + gy as u32 * s, s, s, color);
            }
        }
        self.mark_dirty(base_x, base_y, self.cell_w, self.cell_h);
    }

    /// Erases `count` cells starting at (col, row), always to the background
    /// colour -- deliberately not draw_glyph(' '), which would paint the
    /// inverted block that reverse video turns a space into.
    fn erase_cells(&mut self, col: u32, row: u32, count: u32) {
        if col >= self.cols || count == 0 {
            return;
        }
        let count = count.min(self.cols - col);
        let (x, y) = (self.pad_x + col * self.cell_w, self.pad_y + row * self.cell_h);
        self.fill_rect(x, y, count * self.cell_w, self.cell_h, BG_COLOR);
        self.mark_dirty(x, y, count * self.cell_w, self.cell_h);
    }

    /// Moves the text area up one row of cells. Only the rows * cell_h
    /// pixels the grid covers take part -- the margins stay blank.
    fn scroll(&mut self) {
        let Some(fb) = self.fb else { return };
        let row_pixels = (fb.pitch * self.cell_h) as usize;
        let text_pixels = row_pixels * self.rows as usize;
        unsafe {
            let top = fb.pixels.add((fb.pitch * self.pad_y) as usize);
            core::ptr::copy(top.add(row_pixels), top, text_pixels - row_pixels);
        }
        self.fill_rect(0, self.pad_y + (self.rows - 1) * self.cell_h, fb.width, self.cell_h, BG_COLOR);
        self.mark_dirty(0, self.pad_y, fb.width, self.rows * self.cell_h);
    }

    fn clear(&mut self) {
        let Some(fb) = self.fb else { return }; // No screen -- the kernel shell's `clear` still calls this.
        self.fill_rect(0, 0, fb.width, fb.height, BG_COLOR);
        self.mark_dirty(0, 0, fb.width, fb.height);
        (self.cursor_col, self.cursor_row) = (0, 0);
        self.reverse_video = false;
        (self.fg, self.bg) = (None, None);
        (self.bold, self.dim) = (false, false);
        self.parse = Parse::Normal;
    }

    /// Parameter n, `fallback` when it was omitted entirely (ESC[H and
    /// ESC[1;1H mean the same thing).
    fn param_or(&self, n: usize, fallback: u32) -> u32 {
        if n >= self.nparams || self.params[n] == 0 { fallback } else { self.params[n] }
    }

    fn reset_attributes(&mut self) {
        self.reverse_video = false;
        (self.fg, self.bg) = (None, None);
        (self.bold, self.dim) = (false, false);
    }

    /// Dispatches a complete CSI sequence on its final byte. Anything not
    /// understood is swallowed silently -- a terminal that prints the raw
    /// bytes of a sequence it doesn't implement is strictly worse than one
    /// that ignores it.
    fn handle_csi(&mut self, fin: u8) {
        let (cols, rows) = (self.cols, self.rows);
        let mode = if self.nparams > 0 { self.params[0] } else { 0 };
        match fin {
            // CUP -- cursor position, 1-based row;col.
            b'H' | b'f' => {
                self.cursor_row = (self.param_or(0, 1) - 1).min(rows - 1);
                self.cursor_col = (self.param_or(1, 1) - 1).min(cols - 1);
            }
            // CUU/CUD/CUF/CUB -- relative cursor moves, clamped.
            b'A' => self.cursor_row = self.cursor_row.saturating_sub(self.param_or(0, 1)),
            b'B' => self.cursor_row = (self.cursor_row + self.param_or(0, 1)).min(rows - 1),
            b'C' => self.cursor_col = (self.cursor_col + self.param_or(0, 1)).min(cols - 1),
            b'D' => self.cursor_col = self.cursor_col.saturating_sub(self.param_or(0, 1)),
            // ED -- erase display. Does not move the cursor.
            b'J' => {
                let (col, row) = (self.cursor_col, self.cursor_row);
                match mode {
                    2 => (0..rows).for_each(|r| self.erase_cells(0, r, cols)),
                    1 => {
                        (0..row).for_each(|r| self.erase_cells(0, r, cols));
                        self.erase_cells(0, row, col + 1);
                    }
                    _ => {
                        self.erase_cells(col, row, cols - col);
                        (row + 1..rows).for_each(|r| self.erase_cells(0, r, cols));
                    }
                }
            }
            // EL -- erase line. Does not move the cursor.
            b'K' => {
                let (col, row) = (self.cursor_col, self.cursor_row);
                match mode {
                    2 => self.erase_cells(0, row, cols),
                    1 => self.erase_cells(0, row, col + 1),
                    _ => self.erase_cells(col, row, cols - col),
                }
            }
            // SGR -- the 16 ANSI colors, bold, dim and reverse.
            b'm' => {
                if self.nparams == 0 {
                    self.reset_attributes();
                }
                for i in 0..self.nparams {
                    match self.params[i] {
                        0 => self.reset_attributes(),
                        1 => self.bold = true,
                        2 => self.dim = true,
                        7 => self.reverse_video = true,
                        22 => (self.bold, self.dim) = (false, false),
                        27 => self.reverse_video = false,
                        p @ 30..=37 => self.fg = Some(p as usize - 30),
                        39 => self.fg = None,
                        p @ 40..=47 => self.bg = Some(p as usize - 40),
                        49 => self.bg = None,
                        p @ 90..=97 => self.fg = Some(p as usize - 90 + 8),
                        p @ 100..=107 => self.bg = Some(p as usize - 100 + 8),
                        _ => {} // Anything else (underline, 256-color, ...) is ignored.
                    }
                }
            }
            // Includes ESC[?25l/h (cursor visibility) -- nothing to do here,
            // since this console draws no cursor of its own.
            _ => {}
        }
    }

    /// Moves down a row, scrolling past the last one.
    fn newline(&mut self) {
        self.cursor_col = 0;
        self.cursor_row += 1;
        if self.cursor_row >= self.rows {
            self.scroll();
            self.cursor_row = self.rows - 1;
        }
    }

    /// Draws one character at the cursor and advances: wrapping immediately
    /// after the last column (no deferred wrap), scrolling past the last row.
    fn put_glyph(&mut self, cp: u32) {
        self.draw_glyph(self.cursor_col, self.cursor_row, cp);
        self.cursor_col += 1;
        if self.cursor_col >= self.cols {
            self.newline();
        }
    }

    fn putc(&mut self, c: u8) {
        match self.parse {
            Parse::Esc => {
                if c == b'[' {
                    self.parse = Parse::Csi;
                    self.nparams = 0;
                    self.csi_private = false;
                    self.params = [0; MAX_PARAMS];
                } else {
                    self.parse = Parse::Normal; // Not a CSI -- drop it rather than print it.
                }
                return;
            }
            Parse::Csi => {
                match c {
                    b'?' if self.nparams == 0 => self.csi_private = true,
                    b'0'..=b'9' => {
                        if self.nparams == 0 {
                            self.nparams = 1;
                        }
                        let p = &mut self.params[self.nparams - 1];
                        *p = p.wrapping_mul(10).wrapping_add((c - b'0') as u32);
                    }
                    b';' => {
                        // An omitted parameter still occupies a slot (ESC[;5H
                        // means "default row, column 5"), so a separator seen
                        // before any digit has to claim slot 0 before opening
                        // the next one.
                        if self.nparams == 0 {
                            self.nparams = 1;
                        }
                        if self.nparams < MAX_PARAMS {
                            self.nparams += 1;
                        }
                    }
                    _ => {
                        if !self.csi_private {
                            self.handle_csi(c);
                        }
                        self.parse = Parse::Normal;
                    }
                }
                return;
            }
            Parse::Normal => {}
        }

        // UTF-8: a lead byte starts a character, continuation bytes finish
        // it; a malformed sequence draws one '?' and is dropped.
        if self.utf8_need > 0 {
            if c & 0xC0 == 0x80 {
                self.utf8_cp = self.utf8_cp << 6 | (c & 0x3F) as u32;
                self.utf8_need -= 1;
                if self.utf8_need == 0 {
                    self.put_glyph(self.utf8_cp);
                }
                return;
            }
            self.utf8_need = 0;
            self.put_glyph(b'?' as u32); // A truncated sequence; then `c` itself.
        }
        if c >= 0x80 {
            if c & 0xE0 == 0xC0 {
                (self.utf8_cp, self.utf8_need) = ((c & 0x1F) as u32, 1);
            } else if c & 0xF0 == 0xE0 {
                (self.utf8_cp, self.utf8_need) = ((c & 0x0F) as u32, 2);
            } else if c & 0xF8 == 0xF0 {
                (self.utf8_cp, self.utf8_need) = ((c & 0x07) as u32, 3);
            } else {
                self.put_glyph(b'?' as u32); // A stray continuation byte.
            }
            return;
        }

        match c {
            0x1b => self.parse = Parse::Esc,
            b'\r' => self.cursor_col = 0,
            // No-op at column 0 -- deliberately not walking back onto the
            // previous line, since nothing tracks where lines actually ended
            // (the caller wrapped mid-word or not). Good enough for the
            // shell's single-line input editing.
            b'\x08' => {
                if self.cursor_col > 0 {
                    self.cursor_col -= 1;
                    self.draw_glyph(self.cursor_col, self.cursor_row, b' ' as u32);
                }
            }
            b'\n' => self.newline(),
            _ => self.put_glyph(c as u32),
        }
    }
}

/// Takes over `fb`: picks the scale from its width, lays out the grid,
/// and clears it.
pub fn init(fb: Framebuffer) {
    let c = console();
    c.scale = if fb.width >= 3200 {
        3
    } else if fb.width >= 1600 {
        2
    } else {
        1
    };
    (c.cell_w, c.cell_h) = (GLYPH_W * c.scale, GLYPH_H * c.scale);
    (c.pad_x, c.pad_y) = (PAD * c.scale, PAD * c.scale);
    c.cols = (fb.width - 2 * c.pad_x) / c.cell_w;
    c.rows = (fb.height - 2 * c.pad_y) / c.cell_h;
    c.fb = Some(fb);
    c.clear();
}

pub fn clear() {
    console().clear();
}

/// The character grid -- what a full-screen program needs from TIOCGWINSZ
/// -- or None with no screen at all (display init failed), where the
/// serial console is the only terminal and the kernel has no idea how big
/// it is.
pub fn size() -> Option<(u32, u32)> {
    let c = console();
    c.fb.map(|_| (c.cols, c.rows))
}

/// One character cell's size in screen pixels (8x16 times the scale).
pub fn cell_size() -> (u32, u32) {
    let c = console();
    (c.cell_w, c.cell_h)
}

pub fn putc(c: u8) {
    console().putc(c);
}

pub fn write(s: &[u8]) {
    let c = console();
    s.iter().for_each(|&b| c.putc(b));
    c.flush();
}

/// Draws text at an arbitrary cell without touching the scrolling cursor
/// -- for the splash's centered logo and animation. Doesn't flush.
pub fn draw_text_at(col: u32, row: u32, s: &str) {
    let c = console();
    for (i, b) in s.bytes().enumerate() {
        c.draw_glyph(col + i as u32, row, b as u32);
    }
}

/// Suppresses the per-character flush sink() normally does, until
/// the matching end_batch() flushes once. A full-screen redraw is
/// thousands of characters, and a virtio-gpu round trip per character
/// makes it unusably slow -- so a whole write() from userland becomes
/// exactly one flush. Nesting is not supported (there's one flag); safe
/// before init().
pub fn begin_batch() {
    console().batching = true;
}

pub fn end_batch() {
    let c = console();
    c.batching = false;
    if c.fb.is_some() && c.batch_dirty {
        c.batch_dirty = false;
        c.flush();
    }
}

/// The kernel log's console output (drivers/serial.rs): draws the byte and
/// flushes it straight away -- every character, not just on '\n', since
/// this also carries interactive input echo, and a typed character that
/// doesn't show up until Enter looks exactly like the keystroke never
/// arrived. That costs a couple of virtqueue round trips per character
/// during bulk log output on virtio-gpu, and a one-cell copy on a GOP
/// framebuffer -- cheap enough not to matter.
pub fn sink(ch: u8) {
    let c = console();
    c.putc(ch);
    if c.batching {
        c.batch_dirty = true; // end_batch() does the one flush.
        return;
    }
    c.flush();
}
