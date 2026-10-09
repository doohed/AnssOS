//! `play` -- an interactive WAV/MP3 player for AnssOS, with a ratatui
//! UI. Plays the files named on argv in order through the virtio-sound
//! driver (kernel/src/drivers/virtio/virtio_snd.c), polling poll_key()
//! between chunks for the playback controls so the loop feeding the
//! device never blocks on the keyboard.
//!
//! This is a `no_std` static library, not a normal Rust binary: there is
//! no Rust std for AnssOS. scripts/build-userland.sh links it with crt0.o
//! (which calls the `main` below exactly as it would a C program's), the
//! hand-written C libc, and minimp3 (c/mp3.c, see source.rs). The
//! `anssos` runtime crate wraps the libc and provides the allocator and
//! panic handler.
//!
//! The crate builds for x86_64-unknown-none, which is soft-float: the
//! little float math ratatui does (layout, gauge ratios) runs in
//! software, and the hot paths (PCM, spectrum) stay integer. minimp3, in
//! C, does use SSE -- safe since the kernel saves/restores each
//! process's FPU state (kernel/src/arch/x86_64/fpu.h).

#![no_std]

extern crate alloc;

mod backend;
mod source;
mod spectrum;
mod ui;

use alloc::string::String;
use core::ffi::{CStr, c_char, c_int};
use core::fmt::Write;

use ratatui::Terminal;

use backend::AnssBackend;
use source::Source;
use spectrum::Spectrum;

/// 4 KiB of PCM per audio_write() -- ~21 ms of 48 kHz stereo; also how
/// often the screen redraws. A multiple of 4, so always whole frames.
const CHUNK_BYTES: usize = 4096;

enum Outcome {
    Finished,
    Quit,
}

#[unsafe(no_mangle)]
pub extern "C" fn main(argc: c_int, argv: *const *const c_char) -> c_int {
    let args: alloc::vec::Vec<*const c_char> = (1..argc as usize).map(|i| unsafe { *argv.add(i) }).collect();
    if args.is_empty() {
        anssos::write_all(1, b"usage: play <file.wav|file.mp3> [more files ...]\n");
        return 1;
    }

    let Some(raw_mode) = anssos::RawMode::enable() else {
        anssos::write_all(1, b"play: cannot put the terminal in raw mode\n");
        return 1;
    };

    let mut terminal = Terminal::new(AnssBackend::new()).unwrap_or_else(|e| match e {});
    let _ = terminal.hide_cursor();
    let _ = terminal.clear();

    // Per-track errors are reported only after the final screen clear --
    // printed right away, the next redraw (or that clear) would wipe them
    // before anyone could read them.
    let mut errors = String::new();
    let mut volume = 100;
    for (i, &path) in args.iter().enumerate() {
        let name = unsafe { CStr::from_ptr(path) }.to_str().unwrap_or("?");
        match play_track(&mut terminal, path, name, i + 1, args.len(), &mut volume) {
            Ok(Outcome::Quit) => break,
            Ok(Outcome::Finished) => {}
            Err(e) => {
                let _ = writeln!(errors, "play: {e}");
            }
        }
    }

    anssos::write_all(1, b"\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    drop(raw_mode);
    anssos::write_all(1, errors.as_bytes());
    anssos::write_all(1, b"play: done\n");
    if errors.is_empty() { 0 } else { 1 }
}

fn play_track(
    terminal: &mut Terminal<AnssBackend>,
    path: *const c_char,
    name: &str,
    track_idx: usize,
    track_count: usize,
    volume: &mut i32,
) -> Result<Outcome, String> {
    let (mut source, info) = Source::open(path, name)?;
    let mut audio = anssos::Audio::open(info.rate, info.channels)
        .ok_or_else(|| alloc::format!("audio_open failed (rate={} channels={})", info.rate, info.channels))?;

    let frame_bytes = info.channels * 2;
    let bytes_per_sec = info.rate * frame_bytes;
    let mut played: u64 = 0;
    let mut paused = false;
    let mut spectrum = Spectrum::new();
    let mut chunk = [0u8; CHUNK_BYTES];

    let draw = |terminal: &mut Terminal<AnssBackend>, spectrum: &Spectrum, paused: bool, volume: i32, played: u64| {
        let view = ui::View {
            name,
            track_idx,
            track_count,
            info: &info,
            elapsed_secs: (played / bytes_per_sec as u64) as u32,
            volume,
            paused,
            levels: &spectrum.levels,
            peaks: &spectrum.peaks,
        };
        let _ = terminal.draw(|frame| ui::render(frame, &view));
    };
    draw(terminal, &spectrum, paused, *volume, played);

    loop {
        // While paused, spin here on the keyboard alone -- nothing is
        // being fed to the device, so there's nothing else to do.
        loop {
            let mut dirty = true;
            match anssos::poll_key() {
                Some(b' ') => paused = !paused,
                Some(b'q') => return Ok(Outcome::Quit),
                Some(b'n') => return Ok(Outcome::Finished),
                Some(b'+') => *volume = (*volume + 10).min(200),
                Some(b'-') => *volume = (*volume - 10).max(0),
                _ => dirty = false,
            }
            if dirty {
                draw(terminal, &spectrum, paused, *volume, played);
            }
            if !paused {
                break;
            }
        }

        let n = source.read_pcm(&mut chunk, frame_bytes);
        if n == 0 {
            return Ok(Outcome::Finished);
        }
        let pcm = &mut chunk[..n];
        apply_volume(pcm, *volume);
        spectrum.update(pcm, info.channels, info.rate);
        if !audio.write(pcm) {
            return Ok(Outcome::Finished);
        }
        played += n as u64;
        draw(terminal, &spectrum, paused, *volume, played);
    }
}

/// Integer volume scaling, 0-200%.
fn apply_volume(pcm: &mut [u8], volume: i32) {
    if volume == 100 {
        return;
    }
    for s in pcm.chunks_exact_mut(2) {
        let v = i16::from_le_bytes([s[0], s[1]]) as i32 * volume / 100;
        s.copy_from_slice(&(v.clamp(i16::MIN as i32, i16::MAX as i32) as i16).to_le_bytes());
    }
}
