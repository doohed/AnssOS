//! A virtual terminal: one pane's screen, kept as a grid of cells and
//! updated by feeding it whatever the pane's programs write.
//!
//! It emulates exactly what the real console understands
//! (kernel/src/console/fbconsole.c), no more and no less -- every
//! program here is written against that console, so a pane has to
//! behave the same:
//!
//! - printable bytes draw at the cursor and advance it, wrapping
//!   *immediately* after the last column (no deferred wrap) and
//!   scrolling once past the last row;
//! - `\n` goes to column 0 of the next row, `\r` to column 0, `\b` steps
//!   back and blanks that cell;
//! - CSI: CUP (`H`/`f`), CUU/CUD/CUF/CUB (`A`-`D`), ED (`J` 0/1/2), EL
//!   (`K` 0/1/2), SGR (`m`: 7 reverse on, 0/27 or none off), and the
//!   private `?25l`/`?25h` cursor visibility (which the real console
//!   ignores, but a pane needs it to know whether to draw a cursor);
//! - anything else is swallowed silently, like the console does.
//!
//! The immediate-wrap rule matters: it's why anssos-tui's backend never
//! writes a terminal's bottom-right cell, and that holds inside a pane
//! only because the pane wraps and scrolls exactly the same way.

use alloc::vec;
use alloc::vec::Vec;

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Cell {
    pub ch: u8,
    /// Reverse video.
    pub rev: bool,
}

const BLANK: Cell = Cell { ch: b' ', rev: false };
const MAX_PARAMS: usize = 8;

enum State {
    Normal,
    Esc,
    Csi,
}

pub struct Vt {
    pub cols: u16,
    pub rows: u16,
    cells: Vec<Cell>,
    pub cx: u16,
    pub cy: u16,
    rev: bool,
    /// Whether the program wants a cursor shown (`?25h`/`?25l`).
    pub cursor_visible: bool,
    state: State,
    params: [u16; MAX_PARAMS],
    nparams: usize,
    private: bool,
}

impl Vt {
    pub fn new(cols: u16, rows: u16) -> Vt {
        let (cols, rows) = (cols.max(1), rows.max(1));
        Vt {
            cols,
            rows,
            cells: vec![BLANK; cols as usize * rows as usize],
            cx: 0,
            cy: 0,
            rev: false,
            cursor_visible: true,
            state: State::Normal,
            params: [0; MAX_PARAMS],
            nparams: 0,
            private: false,
        }
    }

    pub fn cell(&self, x: u16, y: u16) -> Cell {
        self.cells[y as usize * self.cols as usize + x as usize]
    }

    fn set(&mut self, x: u16, y: u16, cell: Cell) {
        let i = y as usize * self.cols as usize + x as usize;
        self.cells[i] = cell;
    }

    /// Blanks `count` cells from (x, y) along the row -- always to plain
    /// background, like the console's erase_cells(), never to a
    /// reverse-video space.
    fn erase(&mut self, x: u16, y: u16, count: u16) {
        for i in x..(x + count).min(self.cols) {
            self.set(i, y, BLANK);
        }
    }

    fn erase_rows(&mut self, from: u16, to: u16) {
        for y in from..to {
            self.erase(0, y, self.cols);
        }
    }

    fn scroll_up(&mut self) {
        let cols = self.cols as usize;
        self.cells.drain(..cols);
        self.cells.extend(core::iter::repeat_n(BLANK, cols));
    }

    fn next_row(&mut self) {
        self.cy += 1;
        if self.cy >= self.rows {
            self.scroll_up();
            self.cy = self.rows - 1;
        }
    }

    fn put(&mut self, ch: u8) {
        self.set(self.cx, self.cy, Cell { ch, rev: self.rev });
        self.cx += 1;
        if self.cx >= self.cols {
            self.cx = 0;
            self.next_row();
        }
    }

    pub fn feed(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.byte(b);
        }
    }

    fn byte(&mut self, b: u8) {
        match self.state {
            State::Esc => {
                if b == b'[' {
                    self.state = State::Csi;
                    self.params = [0; MAX_PARAMS];
                    self.nparams = 0;
                    self.private = false;
                } else {
                    self.state = State::Normal; // not a CSI -- drop it
                }
            }
            State::Csi => match b {
                b'?' if self.nparams == 0 => self.private = true,
                b'0'..=b'9' => {
                    if self.nparams == 0 {
                        self.nparams = 1;
                    }
                    let p = &mut self.params[self.nparams - 1];
                    *p = p.saturating_mul(10).saturating_add((b - b'0') as u16);
                }
                b';' => {
                    // An omitted parameter still takes a slot.
                    if self.nparams == 0 {
                        self.nparams = 1;
                    }
                    if self.nparams < MAX_PARAMS {
                        self.nparams += 1;
                    }
                }
                _ => {
                    if self.private {
                        self.private_csi(b);
                    } else {
                        self.csi(b);
                    }
                    self.state = State::Normal;
                }
            },
            State::Normal => match b {
                0x1b => self.state = State::Esc,
                b'\n' => {
                    self.cx = 0;
                    self.next_row();
                }
                b'\r' => self.cx = 0,
                0x08 => {
                    if self.cx > 0 {
                        self.cx -= 1;
                        self.set(self.cx, self.cy, Cell { ch: b' ', rev: self.rev });
                    }
                }
                b' '..=b'~' => self.put(b),
                _ => {} // other control bytes, and non-ASCII: the console has no glyph
            },
        }
    }

    /// Parameter `n`, or `fallback` if it was omitted (or 0).
    fn param_or(&self, n: usize, fallback: u16) -> u16 {
        if n >= self.nparams || self.params[n] == 0 { fallback } else { self.params[n] }
    }

    fn csi(&mut self, fin: u8) {
        let (cols, rows) = (self.cols, self.rows);
        match fin {
            b'H' | b'f' => {
                self.cy = (self.param_or(0, 1) - 1).min(rows - 1);
                self.cx = (self.param_or(1, 1) - 1).min(cols - 1);
            }
            b'A' => self.cy = self.cy.saturating_sub(self.param_or(0, 1)),
            b'B' => self.cy = self.cy.saturating_add(self.param_or(0, 1)).min(rows - 1),
            b'C' => self.cx = self.cx.saturating_add(self.param_or(0, 1)).min(cols - 1),
            b'D' => self.cx = self.cx.saturating_sub(self.param_or(0, 1)),
            b'J' => match if self.nparams > 0 { self.params[0] } else { 0 } {
                2 => self.erase_rows(0, rows),
                1 => {
                    self.erase_rows(0, self.cy);
                    self.erase(0, self.cy, self.cx + 1);
                }
                _ => {
                    self.erase(self.cx, self.cy, cols - self.cx);
                    self.erase_rows(self.cy + 1, rows);
                }
            },
            b'K' => match if self.nparams > 0 { self.params[0] } else { 0 } {
                2 => self.erase(0, self.cy, cols),
                1 => self.erase(0, self.cy, self.cx + 1),
                _ => self.erase(self.cx, self.cy, cols - self.cx),
            },
            b'm' => {
                if self.nparams == 0 {
                    self.rev = false;
                }
                for &p in &self.params[..self.nparams] {
                    match p {
                        0 | 27 => self.rev = false,
                        7 => self.rev = true,
                        _ => {}
                    }
                }
            }
            _ => {}
        }
    }

    fn private_csi(&mut self, fin: u8) {
        if self.nparams > 0 && self.params[0] == 25 {
            match fin {
                b'h' => self.cursor_visible = true,
                b'l' => self.cursor_visible = false,
                _ => {}
            }
        }
    }
}
