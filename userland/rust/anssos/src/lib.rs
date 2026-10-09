//! The runtime every AnssOS Rust program links against. There is no Rust
//! std for AnssOS, so this provides the few pieces a `no_std` program
//! still needs:
//!
//! - safe wrappers over AnssOS's hand-written C libc (userland/libc.h),
//!   the same syscall wrappers every C program links against -- nothing
//!   here talks to the kernel directly;
//! - the global allocator, on top of the libc's malloc();
//! - the panic handler.
//!
//! A program just depends on this crate; registering the allocator and
//! panic handler happens here, once. See ../Cargo.toml for how programs
//! are built and linked.

#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::{CStr, c_char, c_int, c_long, c_uint, c_ulong, c_void};
use core::fmt;

pub use alloc::ffi::CString;

const O_RDONLY: c_int = 0;
const O_WRONLY: c_int = 1;
const O_CREAT: c_int = 0x40;
const O_TRUNC: c_int = 0x200;
pub const SEEK_SET: c_int = 0;
pub const SEEK_CUR: c_int = 1;
pub const SEEK_END: c_int = 2;

const TIOCGWINSZ: c_ulong = 0x5413;
const TIOCSWINSZ: c_ulong = 0x5414;
const ICANON: c_uint = 0x0002;
const ECHO: c_uint = 0x0008;
const VMIN: usize = 6;
const VTIME: usize = 5;
const TCSANOW: c_int = 0;

/// Linux's struct termios layout -- see kernel/src/drivers/tty.h.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Termios {
    c_iflag: c_uint,
    c_oflag: c_uint,
    c_cflag: c_uint,
    c_lflag: c_uint,
    c_line: u8,
    c_cc: [u8; 19],
}

/// userland/libc.h's struct dirent -- one entry per readdir().
#[repr(C)]
struct Dirent {
    d_type: u8,
    d_name: [c_char; 64],
}
const DT_DIR: u8 = 4;

/// Opaque: the libc's DIR.
#[repr(C)]
struct Dir {
    _fd: c_int,
}

#[repr(C)]
struct Winsize {
    ws_row: u16,
    ws_col: u16,
    ws_xpixel: u16,
    ws_ypixel: u16,
}

unsafe extern "C" {
    fn write(fd: c_int, buf: *const c_void, len: c_ulong) -> c_long;
    #[link_name = "read"]
    fn c_read(fd: c_int, buf: *mut c_void, len: c_ulong) -> c_long;
    fn open(path: *const c_char, flags: c_int) -> c_int;
    #[link_name = "close"]
    fn c_close(fd: c_int) -> c_int;
    fn lseek(fd: c_int, offset: c_long, whence: c_int) -> c_long;
    #[link_name = "chdir"]
    fn c_chdir(path: *const c_char) -> c_int;
    #[link_name = "getcwd"]
    fn c_getcwd(buf: *mut c_char, size: c_ulong) -> c_int;
    fn opendir(path: *const c_char) -> *mut Dir;
    fn readdir(dir: *mut Dir) -> *mut Dirent;
    fn closedir(dir: *mut Dir) -> c_int;
    fn ioctl(fd: c_int, request: c_ulong, argp: *mut c_void) -> c_long;
    fn tcgetattr(fd: c_int, t: *mut Termios) -> c_int;
    fn tcsetattr(fd: c_int, optional_actions: c_int, t: *const Termios) -> c_int;
    fn audio_open(rate_hz: c_uint, channels: c_uint) -> c_int;
    fn audio_write(buf: *const c_void, len: c_uint) -> c_long;
    fn audio_close() -> c_int;
    #[link_name = "poll_key"]
    fn c_poll_key() -> c_int;
    #[link_name = "sched_yield"]
    fn c_sched_yield() -> c_int;
    #[link_name = "pipe"]
    fn c_pipe(fds: *mut c_int) -> c_int;
    #[link_name = "fork"]
    fn c_fork() -> c_int;
    #[link_name = "execve"]
    fn c_execve(path: *const c_char, argv: *const *const c_char) -> c_int;
    #[link_name = "waitpid"]
    fn c_waitpid(pid: c_int, status: *mut c_int) -> c_int;
    #[link_name = "use_as_stdio"]
    fn c_use_as_stdio(stdin_fd: c_int, stdout_fd: c_int) -> c_int;
    fn malloc(size: usize) -> *mut c_void;
    fn free(ptr: *mut c_void);
    #[link_name = "exit"]
    fn c_exit(code: c_int) -> !;
}

/// Writes all of `bytes` to `fd`. A pipe (stdout inside a tile pane)
/// never blocks: when it's full the write returns 0, so this yields to
/// let the reader drain it and tries again. Gives up on an error (-1,
/// e.g. nobody left to read).
pub fn write_all(fd: c_int, mut bytes: &[u8]) {
    while !bytes.is_empty() {
        let n = unsafe { write(fd, bytes.as_ptr().cast(), bytes.len() as c_ulong) };
        if n < 0 {
            return;
        }
        if n == 0 {
            sched_yield();
            continue;
        }
        bytes = &bytes[n as usize..];
    }
}

/// Hands the rest of this time slice to the next runnable process --
/// for a loop that would otherwise spin on an empty pipe.
pub fn sched_yield() {
    unsafe { c_sched_yield() };
}

/// One non-blocking read from a pipe (or file) fd: `Some(n)` bytes, with
/// `Some(0)` meaning "nothing yet"; `None` once the other end is closed
/// and drained (or on error).
pub fn read_fd(fd: c_int, buf: &mut [u8]) -> Option<usize> {
    let n = unsafe { c_read(fd, buf.as_mut_ptr().cast(), buf.len() as c_ulong) };
    if n < 0 { None } else { Some(n as usize) }
}

pub fn close(fd: c_int) {
    unsafe { c_close(fd) };
}

pub fn exit(code: i32) -> ! {
    unsafe { c_exit(code) }
}

/// Blocking read of one byte from stdin -- in raw mode (RawMode), one
/// keypress. None on EOF or error.
///
/// On the console this blocks in the kernel. Inside a tile pane stdin is
/// a pipe, which never blocks -- an empty read returns 0 -- so this
/// yields and retries until a key arrives. None only once stdin is
/// closed for good.
pub fn read_key() -> Option<u8> {
    let mut b = 0u8;
    loop {
        match unsafe { c_read(0, (&raw mut b).cast(), 1) } {
            1 => return Some(b),
            0 => sched_yield(),
            _ => return None,
        }
    }
}

pub enum KeyPoll {
    Key(u8),
    /// Nothing pressed yet.
    Empty,
    /// stdin is a pipe that's been closed (a tile pane shutting down):
    /// no key will ever come, so the program should quit.
    Closed,
}

/// Non-blocking keypress check.
pub fn poll_key() -> KeyPoll {
    match unsafe { c_poll_key() } {
        -2 => KeyPoll::Closed,
        k if k < 0 => KeyPoll::Empty,
        k => KeyPoll::Key(k as u8),
    }
}

/// Terminal size in (columns, rows), 80x24 if the ioctl fails.
pub fn window_size() -> (u16, u16) {
    let mut ws = Winsize { ws_row: 0, ws_col: 0, ws_xpixel: 0, ws_ypixel: 0 };
    let ok = unsafe { ioctl(0, TIOCGWINSZ, (&raw mut ws).cast()) } == 0;
    if ok && ws.ws_col > 0 && ws.ws_row > 0 { (ws.ws_col, ws.ws_row) } else { (80, 24) }
}

/// Sets this process's window size -- what window_size() (TIOCGWINSZ)
/// will report from now on, here and in every process forked or exec'd
/// from it, instead of the physical console's. tile uses it to tell a
/// pane's programs how big the pane is (kernel/src/drivers/tty.h).
pub fn set_window_size(cols: u16, rows: u16) -> bool {
    let mut ws = Winsize { ws_row: rows, ws_col: cols, ws_xpixel: 0, ws_ypixel: 0 };
    unsafe { ioctl(0, TIOCSWINSZ, (&raw mut ws).cast()) == 0 }
}

/// Both ends of a new pipe: (read end, write end). Pipes never block --
/// see read_fd()/write_all().
pub fn pipe() -> Option<(c_int, c_int)> {
    let mut fds = [0 as c_int; 2];
    if unsafe { c_pipe(fds.as_mut_ptr()) } == 0 { Some((fds[0], fds[1])) } else { None }
}

/// fork(): the child's pid in the parent, 0 in the child, None on
/// failure. The child gets a copy of every open fd -- there's no
/// close-on-exec, so it must close whatever it shouldn't keep.
pub fn fork() -> Option<i32> {
    let pid = unsafe { c_fork() };
    if pid < 0 { None } else { Some(pid) }
}

/// Points this process's stdin/stdout at the given pipe fds (freeing
/// those two fd numbers) -- AnssOS's narrow stand-in for dup2(), meant
/// for a freshly forked child right before exec().
pub fn use_as_stdio(stdin_fd: c_int, stdout_fd: c_int) -> bool {
    unsafe { c_use_as_stdio(stdin_fd, stdout_fd) == 0 }
}

/// Replaces this process with the program at `path`. Only returns if
/// that failed.
pub fn exec(path: &CStr, args: &[&CStr]) {
    let mut argv: Vec<*const c_char> = args.iter().map(|a| a.as_ptr()).collect();
    argv.push(core::ptr::null());
    unsafe { c_execve(path.as_ptr(), argv.as_ptr()) };
}

/// Waits for child `pid` to exit; its exit status.
pub fn waitpid(pid: i32) -> i32 {
    let mut status: c_int = -1;
    unsafe { c_waitpid(pid, &mut status) };
    status
}

/// Puts the console in raw mode (no line buffering, no echo); restores
/// the original settings when dropped.
pub struct RawMode(Termios);

impl RawMode {
    pub fn enable() -> Option<RawMode> {
        let mut orig = Termios { c_iflag: 0, c_oflag: 0, c_cflag: 0, c_lflag: 0, c_line: 0, c_cc: [0; 19] };
        if unsafe { tcgetattr(0, &mut orig) } != 0 {
            return None;
        }
        let mut raw = orig;
        raw.c_lflag &= !(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if unsafe { tcsetattr(0, TCSANOW, &raw) } != 0 {
            return None;
        }
        Some(RawMode(orig))
    }
}

impl Drop for RawMode {
    fn drop(&mut self) {
        unsafe { tcsetattr(0, TCSANOW, &self.0) };
    }
}

/// Changes the working directory. Fails on anything that isn't a
/// directory -- which makes it the reliable "is this a directory?" test,
/// since open() succeeds on directories too.
pub fn chdir(path: &CStr) -> bool {
    unsafe { c_chdir(path.as_ptr()) == 0 }
}

/// The working directory as an absolute, canonical path.
pub fn getcwd() -> Option<String> {
    let mut buf = [0 as c_char; 256];
    if unsafe { c_getcwd(buf.as_mut_ptr(), buf.len() as c_ulong) } != 0 {
        return None;
    }
    let s = unsafe { CStr::from_ptr(buf.as_ptr()) };
    s.to_str().ok().map(String::from)
}

pub struct DirEntry {
    pub name: String,
    pub is_dir: bool,
}

/// Lists a directory's entries, in the VFS's own order. None if it
/// can't be opened.
pub fn read_dir(path: &CStr) -> Option<Vec<DirEntry>> {
    let dir = unsafe { opendir(path.as_ptr()) };
    if dir.is_null() {
        return None;
    }
    let mut entries = Vec::new();
    loop {
        let e = unsafe { readdir(dir) };
        if e.is_null() {
            break;
        }
        let e = unsafe { &*e };
        let name = unsafe { CStr::from_ptr(e.d_name.as_ptr()) };
        entries.push(DirEntry { name: String::from_utf8_lossy(name.to_bytes()).into(), is_dir: e.d_type == DT_DIR });
    }
    unsafe { closedir(dir) };
    Some(entries)
}

/// An open file descriptor, closed on drop.
pub struct File(c_int);

impl File {
    pub fn open(path: &CStr) -> Option<File> {
        let fd = unsafe { open(path.as_ptr(), O_RDONLY) };
        if fd < 0 { None } else { Some(File(fd)) }
    }

    /// Opens for writing, creating the file or truncating it to empty.
    /// O_TRUNC matters: the kernel's write path only ever extends a file,
    /// so without it a shorter save would leave the old tail behind.
    pub fn create(path: &CStr) -> Option<File> {
        let fd = unsafe { open(path.as_ptr(), O_WRONLY | O_CREAT | O_TRUNC) };
        if fd < 0 { None } else { Some(File(fd)) }
    }

    /// Reads up to `buf.len()` bytes; 0 at end of file or on error.
    pub fn read(&mut self, buf: &mut [u8]) -> usize {
        let n = unsafe { c_read(self.0, buf.as_mut_ptr().cast(), buf.len() as c_ulong) };
        if n <= 0 { 0 } else { n as usize }
    }

    /// Writes all of `bytes`; false if the write fell short.
    pub fn write_all(&mut self, mut bytes: &[u8]) -> bool {
        while !bytes.is_empty() {
            let n = unsafe { write(self.0, bytes.as_ptr().cast(), bytes.len() as c_ulong) };
            if n <= 0 {
                return false;
            }
            bytes = &bytes[n as usize..];
        }
        true
    }

    /// Fills as much of `buf` as the file has left; returns the count.
    pub fn read_full(&mut self, buf: &mut [u8]) -> usize {
        let mut got = 0;
        while got < buf.len() {
            let n = self.read(&mut buf[got..]);
            if n == 0 {
                break;
            }
            got += n;
        }
        got
    }

    pub fn seek(&mut self, offset: i64, whence: c_int) -> i64 {
        unsafe { lseek(self.0, offset as c_long, whence) as i64 }
    }
}

impl Drop for File {
    fn drop(&mut self) {
        unsafe { c_close(self.0) };
    }
}

/// The single virtio-sound playback stream; closed on drop.
pub struct Audio;

impl Audio {
    pub fn open(rate: u32, channels: u32) -> Option<Audio> {
        if unsafe { audio_open(rate, channels) } == 0 { Some(Audio) } else { None }
    }

    /// Sends S16LE PCM, blocking until the device has consumed it.
    pub fn write(&mut self, pcm: &[u8]) -> bool {
        unsafe { audio_write(pcm.as_ptr().cast(), pcm.len() as c_uint) >= 0 }
    }
}

impl Drop for Audio {
    fn drop(&mut self) {
        unsafe { audio_close() };
    }
}

/// `core::fmt` sink straight to a file descriptor -- `write!` without
/// allocating, which the panic handler relies on.
pub struct FdWriter(pub c_int);

impl fmt::Write for FdWriter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        write_all(self.0, s.as_bytes());
        Ok(())
    }
}

/// Rust's global allocator, on top of the C libc's brk-backed malloc().
/// That malloc only promises its own HEAP_ALIGN, so every allocation is
/// over-sized by `align` bytes and the pointer malloc() really returned
/// is stashed in the 8 bytes right below the aligned block, for dealloc.
pub struct LibcAlloc;

unsafe impl GlobalAlloc for LibcAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let align = layout.align().max(8);
        let raw = unsafe { malloc(layout.size() + align + 8) } as usize;
        if raw == 0 {
            return core::ptr::null_mut();
        }
        let aligned = (raw + 8 + align - 1) & !(align - 1);
        unsafe { *((aligned - 8) as *mut usize) = raw };
        aligned as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        let raw = unsafe { *((ptr as usize - 8) as *const usize) };
        unsafe { free(raw as *mut c_void) };
    }
}

#[global_allocator]
static ALLOC: LibcAlloc = LibcAlloc;

/// Puts the console back in a usable state before reporting: plain
/// attributes, cursor shown, on a fresh line -- a panic mid-frame in a
/// full-screen program would otherwise leave reverse video on and the
/// cursor hidden. The program's screen itself is left as it was.
#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    use core::fmt::Write;
    write_all(1, b"\x1b[0m\x1b[?25h\r\n");
    let _ = writeln!(FdWriter(1), "panic: {info}");
    exit(101)
}
