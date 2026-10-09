//! Drawing scarf.
//!
//! ```text
//!  /docs/                 # notes.txt [+]                         <- pane titles: the focused
//!                         #                                          one is a solid bar
//!                         #
//!    ../                  #   1 hello world
//!  #######################                                        <- the selection: a solid
//!  ## sub/ ###############    2 second line                          band, text centered
//!  #######################
//!    notes.txt            #   3
//!  NORMAL                                          ln 2/40  col 5  <- solid status bar, mode
//!  :w                                                                 chip cut out of it
//! ```
//!
//! (`#` is a solid cell.) Same visual language as `play`: the console
//! only does reverse video, and a reversed space is a solid cell, so
//! the focused pane's title, the divider, the status bar and the cursor
//! are all solid. The divider is a solid column rather than `|`, which
//! font8x8_basic draws as a broken bar. Which pane has focus is shown
//! twice over -- its title goes solid, and the sidebar's selection is a
//! full solid row only while the sidebar has focus (`>` otherwise).
//!
//! The 8x8 font has no leading, so adjacent text lines touch. On a tall
//! console (LINE_GAP_MIN_ROWS or more) every text and sidebar line gets
//! a blank row after it, as above; a small terminal stays single-spaced
//! rather than halving what fits. Double-spaced, the sidebar selection
//! also takes the blank rows either side of its entry, so its text sits
//! centered in a 3-row band instead of filling a tight 8-pixel strip --
//! and two blank rows under the titles keep the first entry's band from
//! merging into the title bar.
//!
//! Ratatui diffs every frame against the last, so only changed cells
//! reach the console -- the C version's hand-maintained "sidebar dirty"
//! tracking and `ESC[K` tricks aren't needed.

use alloc::format;
use alloc::string::String;

use anssos_tui::solid;
use ratatui::Frame;
use ratatui::buffer::Buffer;
use ratatui::layout::Rect;
use ratatui::style::Style;

use crate::editor::{Editor, Focus, Mode};

const SIDEBAR_W: u16 = 28;
/// Console height from which lines are double-spaced.
const LINE_GAP_MIN_ROWS: u16 = 50;

/// Where everything goes for one terminal size and sidebar state.
pub struct Layout {
    sidebar: Option<Rect>,
    divider: Option<Rect>,
    editor: Rect,
    /// Rows per text line: 2 (a blank row between lines) or 1.
    pitch: u16,
    /// Text lines that fit in the editor / entries in the sidebar.
    pub text_rows: u16,
    pub list_rows: u16,
    pub text_cols: u16,
    gutter: u16,
    status: Rect,
    message: Rect,
}

impl Layout {
    pub fn new(area: Rect, ed: &Editor) -> Layout {
        let row = |y: u16| Rect { x: area.x, y, width: area.width, height: 1 };
        let status = row(area.bottom().saturating_sub(2));
        let message = row(area.bottom().saturating_sub(1));
        let panes = Rect { height: area.height.saturating_sub(2), ..area };

        // The sidebar never crowds out the text.
        let (sidebar, divider, editor) = if ed.sidebar_visible && panes.width >= 24 {
            let w = SIDEBAR_W.min(panes.width / 3).max(8);
            let sidebar = Rect { width: w, ..panes };
            let divider = Rect { x: panes.x + w, width: 1, ..panes };
            let editor = Rect { x: panes.x + w + 1, width: panes.width - w - 1, ..panes };
            (Some(sidebar), Some(divider), editor)
        } else {
            (None, None, panes)
        };

        // Line-number gutter: right-aligned numbers, then a space.
        let digits = digits(ed.buf.lines.len()).max(3);
        let gutter = (digits + 2).min(editor.width / 2);
        let pitch = if area.height >= LINE_GAP_MIN_ROWS { 2 } else { 1 };
        // Below the title row (and, double-spaced, the gap after it).
        let lines = panes.height.saturating_sub(top_margin(pitch)) / pitch;
        Layout {
            sidebar,
            divider,
            editor,
            pitch,
            text_rows: lines,
            list_rows: lines,
            text_cols: editor.width.saturating_sub(gutter),
            gutter,
            status,
            message,
        }
    }
}

fn digits(mut n: usize) -> u16 {
    let mut d = 1;
    while n >= 10 {
        n /= 10;
        d += 1;
    }
    d
}

pub fn render(frame: &mut Frame, ed: &Editor, lay: &Layout) {
    let buf = frame.buffer_mut();
    if let (Some(sidebar), Some(divider)) = (lay.sidebar, lay.divider) {
        draw_sidebar(buf, sidebar, lay, ed);
        fill(buf, divider, solid());
    }
    draw_editor(buf, lay, ed);
    draw_status(buf, lay.status, ed);
    draw_message(buf, lay.message, ed);
}

fn fill(buf: &mut Buffer, area: Rect, style: Style) {
    for y in area.top()..area.bottom() {
        buf.set_stringn(area.x, y, " ".repeat(area.width as usize), area.width as usize, style);
    }
}

/// A pane's first row: solid when the pane has focus, plain otherwise.
fn title(buf: &mut Buffer, area: Rect, text: &str, focused: bool) {
    if area.height == 0 {
        return;
    }
    let style = if focused { solid() } else { Style::new() };
    let row = Rect { height: 1, ..area };
    fill(buf, row, style);
    buf.set_stringn(area.x + 1, area.y, text, area.width.saturating_sub(1) as usize, style);
}

/// One line of file text as the console can show it: printable ASCII
/// as-is, a tab as one space, anything else as `?`.
fn shown(b: u8) -> char {
    match b {
        b' '..=b'~' => b as char,
        b'\t' => ' ',
        _ => '?',
    }
}

/// Rows from a pane's top to its first line: the title, plus two blank
/// rows when double-spaced.
fn top_margin(pitch: u16) -> u16 {
    if pitch > 1 { 3 } else { 1 }
}

/// Screen row of the `i`th text line (or sidebar entry) in a pane.
fn line_y(pane: Rect, lay: &Layout, i: usize) -> u16 {
    pane.y + top_margin(lay.pitch) + i as u16 * lay.pitch
}

fn draw_sidebar(buf: &mut Buffer, area: Rect, lay: &Layout, ed: &Editor) {
    let sb = &ed.sidebar;
    let focused = ed.focus == Focus::Sidebar;
    let mut header = sb.cwd.clone();
    if !header.ends_with('/') {
        header.push('/');
    }
    title(buf, area, &header, focused);

    let list = Rect { height: 1, ..area };
    for (i, entry) in sb.entries.iter().enumerate().skip(sb.offset).take(lay.list_rows as usize) {
        let y = line_y(area, lay, i - sb.offset);
        let selected = i == sb.sel;
        let marker = if selected && !focused { "> " } else { "  " };
        let slash = if entry.is_dir { "/" } else { "" };
        let text = format!("{marker}{}{slash}", entry.name);
        let style = if selected && focused { solid() } else { Style::new() };
        if selected && focused {
            // Double-spaced: the band takes the blank rows either side.
            let band = if lay.pitch > 1 { Rect { y: y - 1, height: 3, ..list } } else { Rect { y, height: 1, ..list } };
            fill(buf, band, style);
        }
        buf.set_stringn(list.x, y, text, list.width as usize, style);
    }
}

fn draw_editor(buf: &mut Buffer, lay: &Layout, ed: &Editor) {
    let area = lay.editor;
    let focused = ed.focus == Focus::Editor;
    let name = if ed.buf.filename.is_empty() { "[no name]" } else { ed.buf.filename.as_str() };
    let dirty = if ed.buf.dirty { " [+]" } else { "" };
    title(buf, area, &format!("{name}{dirty}"), focused);

    let show_cursor = focused && ed.mode != Mode::Command;
    let text_x = area.x + lay.gutter;
    for screen_row in 0..lay.text_rows as usize {
        let y = line_y(area, lay, screen_row);
        let n = ed.rowoff + screen_row;
        let Some(line) = ed.buf.lines.get(n) else {
            break; // past the end of the file: the gutter's numbers stop
        };

        // Line number; the cursor's line is solid while the editor has
        // focus -- a "cursorline" without color.
        let number = format!("{:>w$} ", n + 1, w = (lay.gutter - 1) as usize);
        let number_style = if focused && n == ed.cy { solid() } else { Style::new() };
        buf.set_stringn(area.x, y, number, lay.gutter as usize, number_style);

        let visible: String =
            line.iter().skip(ed.coloff).take(lay.text_cols as usize).map(|&b| shown(b)).collect();
        buf.set_string(text_x, y, visible, Style::new());

        // The cursor is a solid cell -- including one past the end of the
        // line (insert mode, or an empty line), so it's always visible.
        if show_cursor && n == ed.cy && ed.cx >= ed.coloff {
            let col = (ed.cx - ed.coloff) as u16;
            if col < lay.text_cols {
                let under = line.get(ed.cx).map_or(' ', |&b| shown(b));
                buf.set_string(text_x + col, y, under.encode_utf8(&mut [0; 4]), solid());
            }
        }
    }
}

/// Solid bar: a mode chip cut out of it on the left, position right.
fn draw_status(buf: &mut Buffer, area: Rect, ed: &Editor) {
    fill(buf, area, solid());
    let chip = match (ed.mode, ed.focus) {
        (Mode::Insert, _) => " INSERT ",
        (Mode::Command, _) => " COMMAND ",
        (Mode::Normal, Focus::Sidebar) => " FILES ",
        (Mode::Normal, Focus::Editor) => " NORMAL ",
    };
    buf.set_string(area.x, area.y, chip, Style::new());

    let pos = format!("ln {}/{}  col {} ", ed.cy + 1, ed.buf.lines.len(), ed.cx + 1);
    let x = area.right().saturating_sub(pos.len() as u16);
    if x > area.x + chip.len() as u16 {
        buf.set_string(x, area.y, pos, solid());
    }
}

fn draw_message(buf: &mut Buffer, area: Rect, ed: &Editor) {
    if ed.mode == Mode::Command {
        let line = format!(":{}", ed.cmd);
        let len = line.len() as u16;
        buf.set_stringn(area.x, area.y, line, area.width as usize, Style::new());
        if len < area.width {
            buf.set_string(area.x + len, area.y, " ", solid());
        }
    } else {
        buf.set_stringn(area.x, area.y, &ed.message, area.width as usize, Style::new());
    }
}
