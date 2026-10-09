//! ratatui on AnssOS: the console backend (backend.rs) plus the setup,
//! teardown and one style every full-screen program here shares.
//!
//! The console (kernel/src/console/fbconsole.c) is ASCII-only and its
//! only style is reverse video -- see backend.rs for what that means for
//! drawing. The one trick that makes it look like more than a terminal
//! dump: a reverse-video *space* is a solid cell (`solid()`), so bars,
//! title strips and fills render as solid blocks.

#![no_std]

extern crate alloc;

mod backend;

pub use backend::AnssBackend;

use ratatui::Terminal;
use ratatui::style::Style;

pub type Term = Terminal<AnssBackend>;

/// A terminal on the console, cleared and with the cursor hidden --
/// programs draw their own cursor if they need one. Pair with
/// `restore()` on the way out. The caller puts the console in raw mode
/// (anssos::RawMode) first.
pub fn init() -> Term {
    let mut terminal = Terminal::new(AnssBackend::new()).unwrap_or_else(|e| match e {});
    let _ = terminal.hide_cursor();
    let _ = terminal.clear();
    terminal
}

/// Leaves the console the way the shell expects it: normal video,
/// cleared, cursor home and visible. Without this the prompt inherits
/// reverse video and an invisible cursor.
pub fn restore() {
    anssos::write_all(1, b"\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
}

/// Reverse video: on a space, a solid cell.
pub fn solid() -> Style {
    Style::new().reversed()
}
