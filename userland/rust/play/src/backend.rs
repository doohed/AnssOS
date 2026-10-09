//! A ratatui backend for AnssOS's framebuffer console
//! (kernel/src/console/fbconsole.c). The built-in backends (crossterm,
//! termion, ...) need std; this one only needs `write()` on fd 1.
//!
//! The console understands a small ANSI subset, so that's all this
//! emits: CUP (`ESC[row;colH`), ED/EL (`ESC[nJ`, `ESC[nK`) and SGR
//! reverse video (`ESC[7m`/`ESC[0m`) -- no colors. Its font
//! (font8x8_basic) is ASCII-only, so any non-ASCII symbol a widget
//! draws is mapped to an ASCII stand-in; the UI (ui.rs) configures its
//! widgets with ASCII symbol sets so that's only ever a fallback.
//!
//! One console quirk shapes `size()`: writing the last column wraps the
//! cursor immediately (no deferred wrap), so writing the bottom-right
//! cell would scroll the whole screen up a line. The backend therefore
//! reports one row fewer than the console has and never touches its
//! last row.

use alloc::vec::Vec;
use core::convert::Infallible;
use core::fmt::Write;

use ratatui::backend::{Backend, ClearType, WindowSize};
use ratatui::buffer::Cell;
use ratatui::layout::{Position, Size};
use ratatui::style::Modifier;


pub struct AnssBackend {
    out: Vec<u8>,
    /// Where the console's cursor is after the last byte queued in `out`
    /// -- lets consecutive cells on a row skip the CUP sequence.
    cursor: Option<(u16, u16)>,
    reversed: bool,
}

impl AnssBackend {
    pub fn new() -> Self {
        AnssBackend { out: Vec::with_capacity(16 * 1024), cursor: None, reversed: false }
    }

    fn goto(&mut self, x: u16, y: u16) {
        let _ = write!(Bytes(&mut self.out), "\x1b[{};{}H", y + 1, x + 1);
        self.cursor = Some((x, y));
    }

    fn set_reversed(&mut self, on: bool) {
        if on != self.reversed {
            self.out.extend_from_slice(if on { b"\x1b[7m" } else { b"\x1b[0m" });
            self.reversed = on;
        }
    }
}

/// Lets `write!` format straight into the output byte buffer.
struct Bytes<'a>(&'a mut Vec<u8>);

impl Write for Bytes<'_> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        self.0.extend_from_slice(s.as_bytes());
        Ok(())
    }
}

/// ASCII stand-in for a cell's symbol.
fn ascii(symbol: &str) -> u8 {
    let mut chars = symbol.chars();
    let c = match (chars.next(), chars.next()) {
        (None, _) => return b' ',
        (Some(c), None) => c,
        _ => return b'?', // a grapheme cluster -- no way to show it
    };
    match c {
        ' '..='~' => c as u8,
        '─' | '━' | '═' => b'-',
        '│' | '┃' | '║' => b'|',
        '┌' | '┐' | '└' | '┘' | '├' | '┤' | '┬' | '┴' | '┼' | '╭' | '╮' | '╯' | '╰' => b'+',
        '█' | '▇' | '▆' | '▅' | '▄' | '▃' | '▂' | '▁' | '■' => b'#',
        '…' => b'.',
        _ => b'?',
    }
}

impl Backend for AnssBackend {
    type Error = Infallible;

    fn draw<'a, I>(&mut self, content: I) -> Result<(), Infallible>
    where
        I: Iterator<Item = (u16, u16, &'a Cell)>,
    {
        for (x, y, cell) in content {
            if self.cursor != Some((x, y)) {
                self.goto(x, y);
            }
            self.set_reversed(cell.modifier.contains(Modifier::REVERSED));
            self.out.push(ascii(cell.symbol()));
            self.cursor = Some((x + 1, y));
        }
        Ok(())
    }

    fn hide_cursor(&mut self) -> Result<(), Infallible> {
        self.out.extend_from_slice(b"\x1b[?25l");
        Ok(())
    }

    fn show_cursor(&mut self) -> Result<(), Infallible> {
        self.out.extend_from_slice(b"\x1b[?25h");
        Ok(())
    }

    fn get_cursor_position(&mut self) -> Result<Position, Infallible> {
        let (x, y) = self.cursor.unwrap_or((0, 0));
        Ok(Position { x, y })
    }

    fn set_cursor_position<P: Into<Position>>(&mut self, position: P) -> Result<(), Infallible> {
        let p = position.into();
        self.goto(p.x, p.y);
        Ok(())
    }

    fn clear(&mut self) -> Result<(), Infallible> {
        self.set_reversed(false);
        self.out.extend_from_slice(b"\x1b[2J");
        Ok(())
    }

    fn clear_region(&mut self, clear_type: ClearType) -> Result<(), Infallible> {
        self.set_reversed(false);
        self.out.extend_from_slice(match clear_type {
            ClearType::All => b"\x1b[2J",
            ClearType::AfterCursor => b"\x1b[0J",
            ClearType::BeforeCursor => b"\x1b[1J",
            ClearType::CurrentLine => b"\x1b[2K",
            ClearType::UntilNewLine => b"\x1b[0K",
        });
        Ok(())
    }

    fn size(&self) -> Result<Size, Infallible> {
        let (cols, rows) = anssos::window_size();
        Ok(Size { width: cols, height: rows.saturating_sub(1).max(1) })
    }

    fn window_size(&mut self) -> Result<WindowSize, Infallible> {
        Ok(WindowSize { columns_rows: self.size()?, pixels: Size { width: 0, height: 0 } })
    }

    fn flush(&mut self) -> Result<(), Infallible> {
        anssos::write_all(1, &self.out);
        self.out.clear();
        Ok(())
    }
}
