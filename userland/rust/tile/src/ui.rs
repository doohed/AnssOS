//! Laying out and drawing tile's screen.
//!
//! ```text
//!  tile                                               pane 1 of 4     <- solid header
//!  # pane 1 ####################### - pane 2 -----------------------  <- pane titles: focused
//!  sh:/> ls                        # sh:/>                               solid, others a rule
//!    bin/                          #
//!  sh:/> _                         #                                  <- solid divider
//!  - pane 3 ----------------------- - pane 4 -----------------------
//!  ...                             # ...
//!  ^B 1-4  focus   ^B o  next   ^B ^B  send ^B   ^B q  quit           <- keycaps reversed
//! ```
//!
//! (`#` is a solid cell.) Same visual language as play and scarf: tile's
//! own chrome is monochrome, reverse video only, and a reversed space is
//! a solid cell. What runs *in* a pane keeps its colors (vt.rs). Unfocused pane titles are a `-` rule rather than plain
//! text, so the bottom row of panes stays visibly separated from the top.
//! Three panes put the third across the full bottom row.

use alloc::format;
use alloc::vec::Vec;

use anssos_tui::solid;
use ratatui::Frame;
use ratatui::buffer::Buffer;
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};

use crate::pane::Pane;
use crate::vt::Attr;

/// Where pane `i` goes: its title row plus the content area below it.
pub struct Slot {
    pub title: Rect,
    pub content: Rect,
}

/// The grid for `n` panes in `area` (the whole terminal): header row
/// on top, key-hint row at the bottom, panes between, with a one-column
/// divider between side-by-side panes.
pub fn layout(area: Rect, n: usize) -> (Vec<Slot>, Vec<Rect>) {
    let grid = Rect { y: area.y + 1, height: area.height.saturating_sub(2), ..area };
    let rows = if n <= 2 { 1 } else { 2 };
    let mut slots = Vec::new();
    let mut dividers = Vec::new();

    for r in 0..rows {
        let y = grid.y + grid.height / rows * r;
        let h = if r == rows - 1 { grid.bottom() - y } else { grid.height / rows };
        // Panes in this row: 1 or 2 (a 3-pane grid's bottom row has one).
        let first = r as usize * 2;
        let in_row = if n == 1 { 1 } else { (n - first).min(2) };
        let cell_w = (grid.width.saturating_sub(in_row as u16 - 1)) / in_row as u16;
        for c in 0..in_row as u16 {
            let x = grid.x + c * (cell_w + 1);
            let w = if c == in_row as u16 - 1 { grid.right() - x } else { cell_w };
            let cell = Rect { x, y, width: w, height: h };
            slots.push(Slot {
                title: Rect { height: 1, ..cell },
                content: Rect { y: y + 1, height: h.saturating_sub(1), ..cell },
            });
            if c + 1 < in_row as u16 {
                dividers.push(Rect { x: x + w, y, width: 1, height: h });
            }
        }
    }
    (slots, dividers)
}

pub fn render(frame: &mut Frame, panes: &[Pane], slots: &[Slot], dividers: &[Rect], focused: usize) {
    let area = frame.area();
    let buf = frame.buffer_mut();

    // Header.
    fill(buf, Rect { height: 1, ..area }, solid());
    buf.set_string(area.x + 1, area.y, "tile", solid());
    let counter = format!("pane {} of {} ", focused + 1, panes.len());
    buf.set_string(area.right().saturating_sub(counter.len() as u16), area.y, counter, solid());

    for d in dividers {
        fill(buf, *d, solid());
    }
    for (i, (pane, slot)) in panes.iter().zip(slots).enumerate() {
        pane_title(buf, slot.title, i, pane, i == focused);
        pane_content(buf, slot.content, pane, i == focused);
    }

    footer(buf, area, panes.len());
}

fn fill(buf: &mut Buffer, area: Rect, style: Style) {
    for y in area.top()..area.bottom() {
        buf.set_stringn(area.x, y, " ".repeat(area.width as usize), area.width as usize, style);
    }
}

fn pane_title(buf: &mut Buffer, area: Rect, i: usize, pane: &Pane, focused: bool) {
    let status = if pane.alive() { "" } else { " [exited]" };
    let label = format!(" pane {}{status} ", i + 1);
    if focused {
        fill(buf, area, solid());
        buf.set_stringn(area.x, area.y, label, area.width as usize, solid());
    } else {
        buf.set_stringn(area.x, area.y, "-".repeat(area.width as usize), area.width as usize, Style::new());
        buf.set_stringn(area.x + 1, area.y, label, area.width.saturating_sub(1) as usize, Style::new());
    }
}

/// Copies the pane's VT into its cell. The focused pane also shows its
/// cursor as a solid cell -- unless the program hid it (full-screen
/// programs draw their own).
fn pane_content(buf: &mut Buffer, area: Rect, pane: &Pane, focused: bool) {
    let vt = &pane.vt;
    for y in 0..vt.rows.min(area.height) {
        for x in 0..vt.cols.min(area.width) {
            let cell = vt.cell(x, y);
            let mut attr = cell.attr;
            if focused && pane.alive() && vt.cursor_visible && x == vt.cx && y == vt.cy {
                attr.rev = !attr.rev;
            }
            if let Some(out) = buf.cell_mut((area.x + x, area.y + y)) {
                out.set_char(cell.ch);
                out.set_style(style_of(attr));
            }
        }
    }
}

/// A VT cell's attributes as a ratatui style (anssos-tui's backend turns
/// it back into the same SGR codes).
fn style_of(a: Attr) -> Style {
    let color = |i: Option<u8>| i.map_or(Color::Reset, Color::Indexed);
    let mut style = Style::new().fg(color(a.fg)).bg(color(a.bg));
    if a.bold {
        style = style.add_modifier(Modifier::BOLD);
    }
    if a.dim {
        style = style.add_modifier(Modifier::DIM);
    }
    if a.rev {
        style = style.add_modifier(Modifier::REVERSED);
    }
    style
}

/// Key hints, htop-style: keycaps reversed, actions plain.
fn footer(buf: &mut Buffer, area: Rect, n: usize) {
    let focus_keys = format!(" ^B 1-{n} ");
    let keys = [
        (focus_keys.as_str(), " focus  "),
        (" ^B o ", " next  "),
        (" ^B ^B ", " send ^B  "),
        (" ^B q ", " quit"),
    ];
    let mut spans = alloc::vec![Span::raw(" ")];
    for (key, action) in keys {
        spans.push(Span::styled(key, solid()));
        spans.push(Span::raw(action));
    }
    buf.set_line(area.x, area.bottom() - 1, &Line::from(spans), area.width);
}
