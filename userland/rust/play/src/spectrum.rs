//! A cava-style spectrum: one fixed-point Goertzel filter per band.
//!
//! Goertzel computes one DFT bin's magnitude with a 2nd-order integer
//! recurrence and a single per-bin coefficient, `2*cos(2*pi*f/fs)` --
//! precomputed on the host as Q15 below, so no trig (or float) is
//! needed at runtime. This crate builds for x86_64-unknown-none, which
//! is soft-float; keeping the per-sample hot loop integer-only matters.

pub const N_BANDS: usize = 20;

/// Bar level range: a band's power bit-length of LEVEL_FLOOR or less is
/// silence, LEVEL_FLOOR + MAX_LEVEL or more is a full bar. Calibrated
/// against real audio for the original C player: silence and quiet
/// passages land under bit-length ~25, a present tone or music bar
/// roughly 27-47.
pub const MAX_LEVEL: u64 = 24;
const LEVEL_FLOOR: i32 = 25;
/// Levels a bar falls per chunk (~21 ms of 48 kHz stereo).
const DECAY: u64 = 3;

/// coeff[i] for 20 log-spaced bands from 60 Hz to 7000 Hz -- 60 77 99
/// 127 163 210 270 346 445 572 735 944 1212 1557 2001 2570 3302 4242
/// 5449 7000 -- is round(2*cos(2*pi*freq[i]/fs) * 32768).
const COEFF_44100: [i64; N_BANDS] = [
    65534, 65532, 65529, 65525, 65518, 65507, 65488, 65456, 65404, 65319, 65177, 64945, 64561, 63929,
    62892, 61191, 58418, 53929, 46759, 35556,
];
const COEFF_48000: [i64; N_BANDS] = [
    65534, 65533, 65530, 65527, 65521, 65511, 65495, 65469, 65425, 65353, 65233, 65037, 64713, 64179,
    63302, 61862, 59510, 55692, 49560, 39896,
];

pub struct Spectrum {
    /// Current bar levels, 0..=MAX_LEVEL.
    pub levels: [u64; N_BANDS],
    mono: [i32; 2048],
}

impl Spectrum {
    pub fn new() -> Self {
        Spectrum { levels: [0; N_BANDS], mono: [0; 2048] }
    }

    /// Updates `levels` from one chunk of S16LE PCM. A bar jumps straight
    /// up to a louder level but falls by only DECAY per chunk -- a cheap
    /// "gravity" that keeps bars from flickering between chunks.
    pub fn update(&mut self, pcm: &[u8], channels: u32, rate: u32) {
        let frame_bytes = channels as usize * 2;
        let frames = (pcm.len() / frame_bytes).min(self.mono.len());
        for i in 0..frames {
            let s = |k: usize| i16::from_le_bytes([pcm[k], pcm[k + 1]]) as i32;
            let base = i * frame_bytes;
            self.mono[i] = if channels == 2 { (s(base) + s(base + 2)) / 2 } else { s(base) };
        }
        let samples = &self.mono[..frames];

        let coeffs = if rate == 48000 { &COEFF_48000 } else { &COEFF_44100 };
        for (level, &coeff) in self.levels.iter_mut().zip(coeffs) {
            let (mut s1, mut s2) = (0i64, 0i64);
            for &x in samples {
                let s = x as i64 + ((coeff * s1) >> 15) - s2;
                s2 = s1;
                s1 = s;
            }
            // Fixed-point rounding can nudge this slightly negative near silence.
            let power = (s1 * s1 + s2 * s2 - ((coeff * s1) >> 15) * s2).max(0) as u64;
            let bits = (u64::BITS - power.leading_zeros()) as i32;
            let target = (bits - LEVEL_FLOOR).clamp(0, MAX_LEVEL as i32) as u64;

            *level = target.max(level.saturating_sub(DECAY));
        }
    }
}
