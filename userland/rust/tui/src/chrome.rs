//! The look the full-screen programs share (scarf, play, tile), taken from
//! sh's prompt: round-ended tabs, thin grey rules, arrow-ended chips and
//! keycaps on a grey band. Callers pick the colors: an accent for
//! whatever has focus or is active, grey for everything else.
//!
//! Text on a colored tab or chip is black and never bold: the console
//! draws bold as the bright color, which would turn black into grey.
//! The round and arrow ends are glyphs from the console's font
//! (crate::console_has()).

use alloc::string::String;

use ratatui::buffer::Buffer;
use ratatui::layout::Rect;
use ratatui::style::{Color, Style};

/// Rules, dividers, secondary text, the backing of chips and keycaps.
pub const GREY: Color = Color::DarkGray;
/// Text on an accent-colored tab or chip.
pub const ON_ACCENT: Color = Color::Black;

pub const TAB_LEFT: &str = "\u{E0B6}"; // solid left round
pub const TAB_RIGHT: &str = "\u{E0B4}"; // solid right round
pub const ARROW_RIGHT: &str = "\u{E0B0}"; // solid right arrow
pub const ARROW_LEFT: &str = "\u{E0B2}"; // solid left arrow

pub fn fg(c: Color) -> Style {
    Style::new().fg(c)
}

/// Text in `color` on a background of `bg`.
pub fn on(color: Color, bg: Color) -> Style {
    Style::new().fg(color).bg(bg)
}

pub fn fill(buf: &mut Buffer, area: Rect, style: Style) {
    for y in area.top()..area.bottom() {
        buf.set_stringn(area.x, y, " ".repeat(area.width as usize), area.width as usize, style);
    }
}

/// Writes `text` at (x, y) without passing column `right`; returns the
/// column after it.
pub fn put(buf: &mut Buffer, x: u16, y: u16, right: u16, text: &str, style: Style) -> u16 {
    if x >= right {
        return x;
    }
    buf.set_stringn(x, y, text, (right - x) as usize, style).0
}

/// A thin grey line from `x` up to `right`.
pub fn rule(buf: &mut Buffer, x: u16, y: u16, right: u16) {
    if x < right {
        put(buf, x, y, right, &"─".repeat((right - x) as usize), fg(GREY));
    }
}

/// A round-ended tab: ` text ` in `text_color` on `color`. Returns the
/// column after it.
pub fn tab(buf: &mut Buffer, x: u16, y: u16, right: u16, text: &str, color: Color, text_color: Color) -> u16 {
    let mut x = put(buf, x, y, right, TAB_LEFT, fg(color));
    // Leave room for the closing end.
    let mut label = String::from(" ");
    label.push_str(text);
    label.push(' ');
    x = put(buf, x, y, right.saturating_sub(1), &label, on(text_color, color));
    put(buf, x, y, right, TAB_RIGHT, fg(color))
}

/// A title row: a tab one column in, then a grey rule to the row's end.
pub fn title(buf: &mut Buffer, row: Rect, text: &str, color: Color, text_color: Color) {
    if row.height == 0 || row.width < 4 {
        return;
    }
    let x = tab(buf, row.x + 1, row.y, row.right(), text, color, text_color);
    rule(buf, x + 1, row.y, row.right());
}

/// Key hints along a row: each key a grey keycap, its action grey text
/// after it.
pub fn key_hints(buf: &mut Buffer, row: Rect, keys: &[(&str, &str)]) {
    let right = row.right();
    let mut x = row.x + 1;
    for (key, action) in keys {
        let mut cap = String::from(" ");
        cap.push_str(key);
        cap.push(' ');
        x = put(buf, x, row.y, right, &cap, on(Color::White, GREY));
        let mut label = String::from(" ");
        label.push_str(action);
        label.push_str("   ");
        x = put(buf, x, row.y, right, &label, fg(Color::Gray));
    }
}
