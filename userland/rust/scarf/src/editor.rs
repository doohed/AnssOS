//! Editor state and key handling: vim-style modes, motions and edits,
//! the `:` command line, and the sidebar's keys.
//!
//! Deliberately not supported, to keep this reviewable: visual mode,
//! registers/yank/put, undo, and search.

use alloc::string::String;
use alloc::vec::Vec;

use crate::buffer::Buffer;
use crate::sidebar::{Activate, Sidebar};

const CTRL_B: u8 = 0x02; // show/hide the sidebar -- the key VS Code uses
const CTRL_E: u8 = 0x05; // move focus between the sidebar and the editor
const ESC: u8 = 0x1b;
const TAB_STOP: usize = 4;
const CMD_MAX: usize = 127;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Normal,
    Insert,
    Command,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Focus {
    Editor,
    Sidebar,
}

pub struct Editor {
    pub buf: Buffer,
    /// Cursor, in file coordinates.
    pub cx: usize,
    pub cy: usize,
    /// Top-left of the editor viewport (scrolling).
    pub rowoff: usize,
    pub coloff: usize,
    pub mode: Mode,
    pub focus: Focus,
    pub sidebar: Sidebar,
    pub sidebar_visible: bool,
    pub message: String,
    pub cmd: String,
    /// A half-typed operator: `d` or `g`.
    pending: Option<u8>,
    pub quit: bool,
}

fn is_word(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}

fn is_enter(c: u8) -> bool {
    c == b'\r' || c == b'\n'
}

fn is_backspace(c: u8) -> bool {
    c == 0x08 || c == 0x7f
}

fn is_printable(c: u8) -> bool {
    (32..127).contains(&c)
}

impl Editor {
    pub fn new(cwd: String) -> Self {
        Editor {
            buf: Buffer::new(),
            cx: 0,
            cy: 0,
            rowoff: 0,
            coloff: 0,
            mode: Mode::Normal,
            focus: Focus::Sidebar,
            sidebar: Sidebar::new(cwd),
            sidebar_visible: true,
            message: String::new(),
            cmd: String::new(),
            pending: None,
            quit: false,
        }
    }

    pub fn set_message(&mut self, msg: &str) {
        self.message.clear();
        self.message.push_str(msg);
    }

    pub fn open(&mut self, path: &str) {
        let msg = self.buf.open(path);
        self.set_message(msg);
        (self.cx, self.cy, self.rowoff, self.coloff) = (0, 0, 0, 0);
    }

    fn save(&mut self) -> bool {
        match self.buf.save() {
            Ok(msg) => {
                // A new file may have appeared in the listing.
                self.sidebar_reload();
                self.set_message(msg);
                true
            }
            Err(msg) => {
                self.set_message(msg);
                false
            }
        }
    }

    fn sidebar_reload(&mut self) {
        let sel = self.sidebar.sel;
        if !self.sidebar.load() {
            self.set_message("cannot open directory");
        }
        self.sidebar.sel = sel.min(self.sidebar.entries.len().saturating_sub(1));
    }

    fn line(&self) -> &Vec<u8> {
        &self.buf.lines[self.cy]
    }

    fn line_mut(&mut self) -> &mut Vec<u8> {
        self.buf.dirty = true;
        &mut self.buf.lines[self.cy]
    }

    /// Keeps the viewport around the cursor, given the text area's size.
    pub fn scroll(&mut self, rows: usize, cols: usize) {
        let (rows, cols) = (rows.max(1), cols.max(1));
        if self.cy < self.rowoff {
            self.rowoff = self.cy;
        }
        if self.cy >= self.rowoff + rows {
            self.rowoff = self.cy + 1 - rows;
        }
        if self.cx < self.coloff {
            self.coloff = self.cx;
        }
        if self.cx >= self.coloff + cols {
            self.coloff = self.cx + 1 - cols;
        }
    }

    /// Normal mode sits *on* a character, insert mode may sit just past
    /// the last one.
    fn clamp_cx(&mut self) {
        let len = self.line().len();
        let max = if self.mode != Mode::Insert && len > 0 { len - 1 } else { len };
        self.cx = self.cx.min(max);
    }

    // ---------- top-level dispatch ----------

    pub fn key(&mut self, c: u8) {
        // The pane shortcuts work from anywhere except while a command
        // line or an insert is in progress, where the byte belongs to
        // whatever's being typed.
        if self.mode == Mode::Normal && c == CTRL_B {
            self.sidebar_visible = !self.sidebar_visible;
            if !self.sidebar_visible {
                self.focus = Focus::Editor;
            }
            self.coloff = 0;
            return;
        }
        if self.mode == Mode::Normal && c == CTRL_E {
            if self.sidebar_visible {
                self.focus = if self.focus == Focus::Editor { Focus::Sidebar } else { Focus::Editor };
            }
            return;
        }
        match (self.mode, self.focus) {
            (Mode::Command, _) => self.key_command(c),
            (Mode::Insert, _) => self.key_insert(c),
            (Mode::Normal, Focus::Sidebar) => self.key_sidebar(c),
            (Mode::Normal, Focus::Editor) => self.key_normal(c),
        }
    }

    fn start_command(&mut self) {
        self.mode = Mode::Command;
        self.cmd.clear();
        self.message.clear();
    }

    // ---------- sidebar ----------

    fn key_sidebar(&mut self, c: u8) {
        match c {
            b'j' => self.sidebar.down(),
            b'k' => self.sidebar.up(),
            b'g' => self.sidebar.first(),
            b'G' => self.sidebar.last(),
            b'l' => self.sidebar_activate(),
            c if is_enter(c) => self.sidebar_activate(),
            b'h' | b'-' => {
                if !self.sidebar.go_parent() {
                    self.set_message("cannot open directory");
                }
            }
            b'r' => {
                // Re-read: files the editor created show up.
                self.sidebar_reload();
                self.set_message("refreshed");
            }
            ESC => self.focus = Focus::Editor,
            b':' => {
                self.focus = Focus::Editor;
                self.start_command();
            }
            _ => {}
        }
    }

    fn sidebar_activate(&mut self) {
        if let Activate::OpenFile(path) = self.sidebar.activate() {
            if self.buf.dirty {
                self.set_message("unsaved changes -- :w first, or :e! to discard");
                return;
            }
            self.open(&path);
            self.focus = Focus::Editor;
        }
    }

    // ---------- normal mode ----------

    fn key_normal(&mut self, c: u8) {
        match self.pending.take() {
            Some(b'd') => {
                match c {
                    b'd' => {
                        self.buf.delete_line(self.cy);
                        self.cy = self.cy.min(self.buf.lines.len() - 1);
                    }
                    b'w' => self.delete_word(),
                    _ => {}
                }
                self.clamp_cx();
                return;
            }
            Some(b'g') => {
                if c == b'g' {
                    (self.cx, self.cy) = (0, 0);
                }
                return;
            }
            _ => {}
        }

        match c {
            b'h' => self.cx = self.cx.saturating_sub(1),
            b'l' => {
                if self.cx + 1 < self.line().len() {
                    self.cx += 1;
                }
            }
            b'j' => {
                if self.cy + 1 < self.buf.lines.len() {
                    self.cy += 1;
                    self.clamp_cx();
                }
            }
            b'k' => {
                if self.cy > 0 {
                    self.cy -= 1;
                    self.clamp_cx();
                }
            }
            b'0' => self.cx = 0,
            b'$' => self.cx = self.line().len().saturating_sub(1),
            b'w' => {
                self.word_forward();
                self.clamp_cx();
            }
            b'b' => {
                self.word_back();
                self.clamp_cx();
            }
            b'G' => {
                self.cy = self.buf.lines.len() - 1;
                self.clamp_cx();
            }
            b'g' | b'd' => self.pending = Some(c),
            b'x' => {
                if self.cx < self.line().len() {
                    let cx = self.cx;
                    self.line_mut().remove(cx);
                }
                self.clamp_cx();
            }
            b'i' => self.mode = Mode::Insert,
            b'a' => {
                if !self.line().is_empty() {
                    self.cx += 1;
                }
                self.mode = Mode::Insert;
            }
            b'A' => {
                self.cx = self.line().len();
                self.mode = Mode::Insert;
            }
            b'o' => {
                self.buf.insert_line(self.cy + 1, Vec::new());
                (self.cx, self.cy) = (0, self.cy + 1);
                self.mode = Mode::Insert;
            }
            b'O' => {
                self.buf.insert_line(self.cy, Vec::new());
                self.cx = 0;
                self.mode = Mode::Insert;
            }
            b':' => self.start_command(),
            _ => {}
        }
    }

    fn word_forward(&mut self) {
        let line = self.line();
        let mut i = self.cx;
        while i < line.len() && is_word(line[i]) {
            i += 1;
        }
        while i < line.len() && !is_word(line[i]) {
            i += 1;
        }
        if i >= line.len() && self.cy + 1 < self.buf.lines.len() {
            (self.cx, self.cy) = (0, self.cy + 1);
        } else {
            self.cx = i;
        }
    }

    fn word_back(&mut self) {
        if self.cx == 0 {
            if self.cy > 0 {
                self.cy -= 1;
                self.cx = self.line().len().saturating_sub(1);
            }
            return;
        }
        let line = self.line();
        let mut i = self.cx - 1;
        while i > 0 && !is_word(line[i]) {
            i -= 1;
        }
        while i > 0 && is_word(line[i - 1]) {
            i -= 1;
        }
        self.cx = i;
    }

    fn delete_word(&mut self) {
        let line = self.line();
        if self.cx >= line.len() {
            return;
        }
        let mut end = self.cx;
        while end < line.len() && is_word(line[end]) {
            end += 1;
        }
        while end < line.len() && !is_word(line[end]) {
            end += 1;
        }
        let cx = self.cx;
        self.line_mut().drain(cx..end);
    }

    // ---------- insert mode ----------

    fn key_insert(&mut self, c: u8) {
        match c {
            ESC => {
                // vim leaves the cursor on the last inserted character.
                self.mode = Mode::Normal;
                self.cx = self.cx.saturating_sub(1);
                self.clamp_cx();
            }
            c if is_enter(c) => {
                let cx = self.cx;
                let tail = self.line_mut().split_off(cx);
                self.buf.insert_line(self.cy + 1, tail);
                (self.cx, self.cy) = (0, self.cy + 1);
            }
            c if is_backspace(c) => {
                if self.cx > 0 {
                    self.cx -= 1;
                    let cx = self.cx;
                    self.line_mut().remove(cx);
                } else if self.cy > 0 {
                    // Join with the previous line.
                    let line = self.buf.lines.remove(self.cy);
                    self.cy -= 1;
                    self.cx = self.line().len();
                    self.line_mut().extend_from_slice(&line);
                }
            }
            b'\t' => {
                for _ in 0..TAB_STOP {
                    self.insert_char(b' ');
                }
            }
            c if is_printable(c) => self.insert_char(c),
            _ => {}
        }
    }

    fn insert_char(&mut self, c: u8) {
        let cx = self.cx;
        self.line_mut().insert(cx, c);
        self.cx += 1;
    }

    // ---------- command line ----------

    fn key_command(&mut self, c: u8) {
        match c {
            ESC => {
                self.mode = Mode::Normal;
                self.cmd.clear();
            }
            c if is_enter(c) => self.run_command(),
            c if is_backspace(c) => {
                if self.cmd.pop().is_none() {
                    self.mode = Mode::Normal;
                }
            }
            c if is_printable(c) && self.cmd.len() < CMD_MAX => self.cmd.push(c as char),
            _ => {}
        }
    }

    fn run_command(&mut self) {
        let cmd = core::mem::take(&mut self.cmd);
        self.mode = Mode::Normal;
        const UNSAVED_EDIT: &str = "unsaved changes -- :w first, or :e! to discard";

        match cmd.as_str() {
            "q" if self.buf.dirty => self.set_message("unsaved changes -- :q! to discard, :wq to save"),
            "q" | "q!" => self.quit = true,
            "w" => {
                self.save();
            }
            "wq" | "x" => {
                if self.save() {
                    self.quit = true;
                }
            }
            _ => {
                if let Some(path) = cmd.strip_prefix("e! ") {
                    self.open(path);
                } else if let Some(path) = cmd.strip_prefix("e ") {
                    if self.buf.dirty {
                        self.set_message(UNSAVED_EDIT);
                    } else {
                        self.open(path);
                    }
                } else if let Some(path) = cmd.strip_prefix("w ") {
                    self.buf.filename = String::from(path);
                    self.save();
                } else {
                    self.set_message("unknown command");
                }
            }
        }
        self.clamp_cx();
    }
}
