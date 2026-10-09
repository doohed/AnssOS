//! The player screen, drawn with ratatui widgets. Every widget gets an
//! ASCII symbol set (borders, bars, gauges): the console's font has no
//! box-drawing or block glyphs, and its only "style" is reverse video.

use alloc::format;
use alloc::vec::Vec;

use ratatui::Frame;
use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::{Style, Stylize};
use ratatui::symbols::{bar, border};
use ratatui::text::Line;
use ratatui::widgets::{Bar, BarChart, BarGroup, Block, LineGauge, Paragraph};

use crate::source::TrackInfo;
use crate::spectrum::{MAX_LEVEL, N_BANDS};

const ASCII_BORDER: border::Set = border::Set {
    top_left: "+",
    top_right: "+",
    bottom_left: "+",
    bottom_right: "+",
    vertical_left: "|",
    vertical_right: "|",
    horizontal_top: "-",
    horizontal_bottom: "-",
};

/// Bar tops in three heights: `_` low in the cell, `=` mid, `#` full.
const ASCII_BARS: bar::Set = bar::Set {
    full: "#",
    seven_eighths: "#",
    three_quarters: "#",
    five_eighths: "=",
    half: "=",
    three_eighths: "=",
    one_quarter: "_",
    one_eighth: "_",
    empty: " ",
};

pub struct View<'a> {
    pub name: &'a str,
    pub track_idx: usize,
    pub track_count: usize,
    pub info: &'a TrackInfo,
    pub elapsed_secs: u32,
    pub volume: i32,
    pub paused: bool,
    pub levels: &'a [u64; N_BANDS],
}

/// Layout solving runs ratatui's constraint solver in (soft-)float math,
/// so it's done once per terminal size rather than every frame.
#[derive(Default)]
pub struct Ui {
    cached: Option<(Rect, [Rect; 5])>,
}

impl Ui {
    fn layout(&mut self, area: Rect) -> [Rect; 5] {
        if let Some((a, rects)) = self.cached {
            if a == area {
                return rects;
            }
        }
        let inner = Block::bordered().inner(area);
        let rects = Layout::vertical([
            Constraint::Length(2), // track + format
            Constraint::Min(4),    // spectrum
            Constraint::Length(1), // progress
            Constraint::Length(1), // volume
            Constraint::Length(1), // state
        ])
        .areas(inner);
        self.cached = Some((area, rects));
        rects
    }

    pub fn render(&mut self, frame: &mut Frame, v: &View) {
        let area = frame.area();
        let [info_area, spectrum_area, progress_area, volume_area, state_area] = self.layout(area);

        let outer = Block::bordered()
            .border_set(ASCII_BORDER)
            .title(Line::from(" AnssOS play ").reversed())
            .title_top(Line::from(format!(" [{}/{}] ", v.track_idx, v.track_count)).right_aligned())
            .title_bottom(Line::from(" space pause | n next | q quit | +/- volume ").centered());
        frame.render_widget(outer, area);

        let channels = if v.info.channels == 1 { "mono" } else { "stereo" };
        let codec = if v.info.mp3_kbps > 0 { format!("MP3 {} kbps", v.info.mp3_kbps) } else { "16-bit PCM".into() };
        let info = Paragraph::new(alloc::vec![
            Line::from(format!("Track:  {}", v.name)),
            Line::from(format!("Format: {} Hz, {channels}, {codec}", v.info.rate)),
        ]);
        frame.render_widget(info, info_area);

        frame.render_widget(spectrum(spectrum_area, v.levels), spectrum_area);

        let total = v.info.total_secs;
        let progress = LineGauge::default()
            .ratio(if total > 0 { (v.elapsed_secs.min(total) as f64) / total as f64 } else { 0.0 })
            .label(format!("{} / {}", clock(v.elapsed_secs), clock(total)))
            .filled_symbol("#")
            .unfilled_symbol("-");
        frame.render_widget(progress, progress_area);

        let volume = LineGauge::default()
            .ratio(v.volume.clamp(0, 200) as f64 / 200.0)
            .label(format!("Volume {:>3}%", v.volume))
            .filled_symbol("#")
            .unfilled_symbol("-");
        frame.render_widget(volume, volume_area);

        let state = if v.paused { Line::from(" PAUSED ").reversed() } else { Line::from("PLAYING") };
        frame.render_widget(Paragraph::new(state), state_area);
    }
}

/// Bars stretched to fill the spectrum box's width -- there are always
/// N_BANDS of them, however wide the terminal is.
fn spectrum(area: Rect, levels: &[u64; N_BANDS]) -> BarChart<'static> {
    let block = Block::bordered().border_set(ASCII_BORDER).title(" spectrum 60 Hz - 7 kHz ");
    let width = block.inner(area).width;
    let gap = if width >= 2 * N_BANDS as u16 { 1 } else { 0 };
    let bar_width = ((width + gap) / N_BANDS as u16).saturating_sub(gap).max(1);
    let bars: Vec<Bar> = levels.iter().map(|&l| Bar::default().value(l).text_value("")).collect();
    BarChart::default()
        .block(block)
        .data(BarGroup::default().bars(&bars))
        .max(MAX_LEVEL)
        .bar_set(ASCII_BARS)
        .bar_width(bar_width)
        .bar_gap(gap)
        .bar_style(Style::default())
}

fn clock(secs: u32) -> alloc::string::String {
    format!("{:02}:{:02}", (secs / 60).min(99), secs % 60)
}
