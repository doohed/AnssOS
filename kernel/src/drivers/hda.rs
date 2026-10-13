//! Intel High Definition Audio: the onboard sound of essentially every PC
//! since 2004 (QEMU: -device intel-hda -device hda-output). Playback
//! only, through the codec's analog outputs -- line out, headphones,
//! speakers. HDMI/DisplayPort audio from a graphics card is skipped: the
//! GPU's display engine has to be told to carry audio, which needs a real
//! GPU driver. Polled, like every other driver here.
//!
//! Same contract as the virtio-sound driver, which drivers/audio.rs picks
//! between: S16LE only, 44100 or 48000 Hz, 1 or 2 channels (mono is
//! played on both sides).
//!
//! Register and verb names follow the HD Audio spec (Intel, rev 1.0a);
//! section numbers are given where it helps.

use super::pci::{self, PciDevice};
use super::timer;
use crate::arch::x86_64::clflush_range;
use crate::mm::pmm::PAGE_SIZE;
use crate::mm::{dma, vmm};
use crate::mmio::Regs;
use crate::sync::Global;

// Controller registers (3.3).
const GCAP: u32 = 0x00;
const GCTL: u32 = 0x08;
const STATESTS: u32 = 0x0E;
const INTCTL: u32 = 0x20;
const CORBLBASE: u32 = 0x40;
const CORBUBASE: u32 = 0x44;
const CORBWP: u32 = 0x48;
const CORBRP: u32 = 0x4A;
const CORBCTL: u32 = 0x4C;
const CORBSIZE: u32 = 0x4E;
const RIRBLBASE: u32 = 0x50;
const RIRBUBASE: u32 = 0x54;
const RIRBWP: u32 = 0x58;
const RINTCNT: u32 = 0x5A;
const RIRBCTL: u32 = 0x5C;
const RIRBSTS: u32 = 0x5D;
const RIRBSIZE: u32 = 0x5E;

// Stream descriptor registers, relative to the descriptor (3.3.35).
const fn sd_base(n: u32) -> u32 {
    0x80 + 0x20 * n
}
const SD_CTL: u32 = 0x00; // 3 bytes: SRST, RUN, ..., stream tag in bits 23:20.
const SD_STS: u32 = 0x03;
const SD_LPIB: u32 = 0x04;
const SD_CBL: u32 = 0x08;
const SD_LVI: u32 = 0x0C;
const SD_FMT: u32 = 0x12;
const SD_BDPL: u32 = 0x18;
const SD_BDPU: u32 = 0x1C;
const SD_CTL_SRST: u8 = 0x01;
const SD_CTL_RUN: u8 = 0x02;

// Verbs (7.3). 12-bit verbs carry an 8-bit payload, 4-bit ones 16 bits.
const VERB_GET_PARAM: u32 = 0xF00;
const VERB_GET_CONN_LIST: u32 = 0xF02;
const VERB_SET_CONN_SELECT: u32 = 0x701;
const VERB_SET_POWER_STATE: u32 = 0x705;
const VERB_SET_STREAM_CHANNEL: u32 = 0x706;
const VERB_SET_PIN_CONTROL: u32 = 0x707;
const VERB_SET_EAPD: u32 = 0x70C;
const VERB_GET_CONFIG_DEFAULT: u32 = 0xF1C;
const VERB4_SET_FORMAT: u32 = 0x2;
const VERB4_SET_AMP: u32 = 0x3;

// Parameters (7.3.4).
const PARAM_VENDOR_ID: u32 = 0x00;
const PARAM_NODE_COUNT: u32 = 0x04;
const PARAM_FUNCTION_TYPE: u32 = 0x05;
const PARAM_WIDGET_CAPS: u32 = 0x09;
const PARAM_PIN_CAPS: u32 = 0x0C;
const PARAM_IN_AMP_CAPS: u32 = 0x0D;
const PARAM_CONN_LIST_LEN: u32 = 0x0E;
const PARAM_OUT_AMP_CAPS: u32 = 0x12;

// Widget types (7.3.4.6).
const WIDGET_OUTPUT: u32 = 0;
const WIDGET_MIXER: u32 = 2;
const WIDGET_SELECTOR: u32 = 3;
const WIDGET_PIN: u32 = 4;

const WCAP_IN_AMP: u32 = 1 << 1;
const WCAP_OUT_AMP: u32 = 1 << 2;
const WCAP_AMP_OVERRIDE: u32 = 1 << 3;
const WCAP_DIGITAL: u32 = 1 << 9;
const WCAP_POWER: u32 = 1 << 10;

const PINCAP_OUT: u32 = 1 << 4;
const PINCAP_HDMI: u32 = 1 << 7;
const PINCAP_EAPD: u32 = 1 << 16;
const PINCAP_DP: u32 = 1 << 24;

const DEVICE_LINE_OUT: u32 = 0;
const DEVICE_SPEAKER: u32 = 1;
const DEVICE_HEADPHONE: u32 = 2;

const MAX_NODES: usize = 256;
const MAX_CONNS: usize = 16;
const MAX_DACS: usize = 8;
const PATH_MAX_DEPTH: usize = 6;

const STREAM_TAG: u8 = 1;
/// ~170 ms of 48 kHz stereo -- the same as virtio-sound's.
const BUFFER_BYTES: u32 = 32768;
const BDL_ENTRIES: u32 = 8;
/// Never write this close behind the DMA: it reads ahead.
const GUARD_BYTES: u64 = 1024;

#[derive(Clone, Copy)]
struct Node {
    caps: u32,
    pin_caps: u32,
    config: u32,
    conn_count: u8,
    conns: [u8; MAX_CONNS],
}

const NODE_EMPTY: Node = Node { caps: 0, pin_caps: 0, config: 0, conn_count: 0, conns: [0; MAX_CONNS] };

#[repr(C)]
struct BdlEntry {
    address: u64,
    length: u32,
    flags: u32,
}

/// The one controller and codec in use.
struct Hda {
    regs: Regs,
    codec: u32,
    afg: u32,
    /// The AFG's amp capabilities, for widgets without their own.
    out_amp_default: u32,
    in_amp_default: u32,
    nodes: [Node; MAX_NODES],
    dacs: [u8; MAX_DACS],
    dac_count: usize,

    corb: *mut u32,
    rirb: *mut u64,
    ring_entries: u32,
    rirb_read: u32,
    /// A codec that stops answering is given up on.
    command_timeouts: u32,

    /// sd_base() of the first output stream.
    stream_base: u32,
    /// BUFFER_BYTES, contiguous.
    buffer: *mut u8,
    buffer_phys: u64,
    bdl: *mut BdlEntry,
    bdl_phys: u64,

    ready: bool,
    open: bool,
    running: bool,
    mono: bool,
    /// Bytes, since open().
    written: u64,
    played: u64,
    last_lpib: u32,
}

static HDA: Global<Hda> = Global::new(Hda {
    regs: Regs(core::ptr::null_mut()),
    codec: 0,
    afg: 0,
    out_amp_default: 0,
    in_amp_default: 0,
    nodes: [NODE_EMPTY; MAX_NODES],
    dacs: [0; MAX_DACS],
    dac_count: 0,
    corb: core::ptr::null_mut(),
    rirb: core::ptr::null_mut(),
    ring_entries: 0,
    rirb_read: 0,
    command_timeouts: 0,
    stream_base: 0,
    buffer: core::ptr::null_mut(),
    buffer_phys: 0,
    bdl: core::ptr::null_mut(),
    bdl_phys: 0,
    ready: false,
    open: false,
    running: false,
    mono: false,
    written: 0,
    played: 0,
    last_lpib: 0,
});

fn hda() -> &'static mut Hda {
    unsafe { HDA.get() }
}

/// `count` zeroed pages of DMA memory, flushed out of the caches:
/// (pointer, physical).
fn dma_pages(count: u64) -> Option<(*mut u8, u64)> {
    let d = dma::alloc(count)?;
    clflush_range(d.virt, (count * PAGE_SIZE) as usize);
    Some((d.virt, d.phys))
}

impl Hda {
    // --- Registers ---

    fn rd8(&self, off: u32) -> u8 {
        self.regs.rd8(off)
    }
    fn rd16(&self, off: u32) -> u16 {
        self.regs.rd16(off)
    }
    fn rd32(&self, off: u32) -> u32 {
        self.regs.rd32(off)
    }
    fn wr8(&self, off: u32, v: u8) {
        self.regs.wr8(off, v)
    }
    fn wr16(&self, off: u32, v: u16) {
        self.regs.wr16(off, v)
    }
    fn wr32(&self, off: u32, v: u32) {
        self.regs.wr32(off, v)
    }

    /// Polls an 8-bit register until (value & mask) == want, up to `ms`.
    fn wait8(&self, off: u32, mask: u8, want: u8, ms: u32) -> bool {
        for waited in 0.. {
            if self.rd8(off) & mask == want {
                return true;
            }
            if waited >= ms {
                return false;
            }
            timer::sleep_ms(1);
        }
        unreachable!()
    }

    // --- Codec commands, through the CORB/RIRB rings (4.4.1) ---

    /// Sends one verb and waits for its response. The controller stops
    /// delivering responses once RINTCNT of them are unacknowledged
    /// (that's what drives its interrupt), so each one is acknowledged in
    /// RIRBSTS -- the interrupt handler's job in a driver that has one.
    fn command(&mut self, nid: u32, verb_and_payload: u32) -> u32 {
        if self.command_timeouts > 0 {
            return 0xFFFF_FFFF;
        }
        let wp = (self.rd16(CORBWP) as u32 + 1) % self.ring_entries;
        let slot = unsafe { self.corb.add(wp as usize) };
        unsafe { slot.write_volatile(self.codec << 28 | nid << 20 | verb_and_payload) };
        clflush_range(slot.cast(), 4);
        self.wr16(CORBWP, wp as u16);

        for tries in 0..20000 + 50 {
            if (self.rd16(RIRBWP) & 0xFF) as u32 % self.ring_entries != self.rirb_read {
                self.rirb_read = (self.rirb_read + 1) % self.ring_entries;
                let entry_ptr = unsafe { self.rirb.add(self.rirb_read as usize) };
                clflush_range(entry_ptr.cast(), 8);
                let entry = unsafe { entry_ptr.read_volatile() };
                self.wr8(RIRBSTS, 0x05); // Response interrupt + overrun flags.
                if (entry >> 36) & 1 != 0 {
                    continue; // Unsolicited (a jack event) -- not our answer.
                }
                return entry as u32;
            }
            if tries >= 20000 {
                timer::sleep_ms(1); // Fast polls first; then up to ~50 ticks.
            }
        }
        self.command_timeouts += 1;
        kprintln!("hda: codec {} stopped answering (verb 0x{:x} to node 0x{:x})", self.codec, verb_and_payload, nid);
        0xFFFF_FFFF
    }

    fn verb(&mut self, nid: u32, v: u32, payload8: u32) -> u32 {
        self.command(nid, v << 8 | (payload8 & 0xFF))
    }

    fn verb4(&mut self, nid: u32, v: u32, payload16: u32) -> u32 {
        self.command(nid, v << 16 | (payload16 & 0xFFFF))
    }

    fn param(&mut self, nid: u32, p: u32) -> u32 {
        self.verb(nid, VERB_GET_PARAM, p)
    }

    // --- Controller ---

    fn reset_controller(&self) -> bool {
        self.wr32(GCTL, self.rd32(GCTL) & !1);
        if !self.wait8(GCTL, 1, 0, 100) {
            return false;
        }
        timer::sleep_ms(1);
        self.wr32(GCTL, self.rd32(GCTL) | 1);
        if !self.wait8(GCTL, 1, 1, 100) {
            return false;
        }
        timer::sleep_ms(10); // Codecs announce themselves within 521 us of reset (4.3).
        true
    }

    fn start_rings(&mut self) -> bool {
        let Some((page, phys)) = dma_pages(1) else { return false };
        self.corb = page.cast();
        self.rirb = unsafe { page.add(2048).cast() };

        let caps = self.rd8(CORBSIZE) >> 4;
        let size_sel: u8 = if caps & 4 != 0 {
            2
        } else if caps & 2 != 0 {
            1
        } else {
            0
        };
        self.ring_entries = match size_sel {
            2 => 256,
            1 => 16,
            _ => 2,
        };

        self.wr8(CORBCTL, 0);
        self.wr8(RIRBCTL, 0);
        self.wr8(CORBSIZE, self.rd8(CORBSIZE) & !3 | size_sel);
        self.wr8(RIRBSIZE, self.rd8(RIRBSIZE) & !3 | size_sel);
        self.wr32(CORBLBASE, phys as u32);
        self.wr32(CORBUBASE, (phys >> 32) as u32);
        self.wr32(RIRBLBASE, (phys + 2048) as u32);
        self.wr32(RIRBUBASE, ((phys + 2048) >> 32) as u32);

        // Read-pointer reset handshake. Some controllers (AMD's among them)
        // clear the bit by themselves, so neither wait is fatal.
        self.wr16(CORBRP, 0x8000);
        for _ in 0..50 {
            if self.rd16(CORBRP) & 0x8000 != 0 {
                break;
            }
            timer::sleep_ms(1);
        }
        self.wr16(CORBRP, 0);
        for _ in 0..50 {
            if self.rd16(CORBRP) & 0x8000 == 0 {
                break;
            }
            timer::sleep_ms(1);
        }
        self.wr16(CORBWP, 0);
        self.wr16(RIRBWP, 0x8000);
        self.wr16(RINTCNT, 1);
        self.wr8(RIRBSTS, 0x05);
        self.rirb_read = 0;

        // DMA run. The RIRB also gets its response-interrupt enable, as in
        // Linux: without it the response flag command() acknowledges is
        // never raised, and some controllers (QEMU's) stop answering after
        // RINTCNT responses. No interrupt reaches the CPU: INTCTL's global
        // enable stays off and PCI INTx is disabled.
        self.wr8(CORBCTL, 0x02);
        self.wr8(RIRBCTL, 0x03);
        true
    }

    // --- Codec graph ---

    fn read_connections(&mut self, nid: u32) {
        let len_param = self.param(nid, PARAM_CONN_LIST_LEN);
        let len = len_param & 0x7F;
        let long_form = (len_param >> 7) & 1 != 0;
        let (per, bits) = if long_form { (2, 16) } else { (4, 8) };
        let mut prev = 0;
        self.nodes[nid as usize].conn_count = 0;

        let mut i = 0;
        while i < len {
            let resp = self.verb(nid, VERB_GET_CONN_LIST, i);
            for j in 0..per {
                if i + j >= len {
                    break;
                }
                let mut e = (resp >> (j * bits)) & ((1 << bits) - 1);
                let range = e & (1 << (bits - 1)) != 0;
                e &= (1 << (bits - 1)) - 1;
                // A range entry means "everything from the previous entry up
                // to this one" (7.3.3.3).
                let from = if range && prev != 0 { prev + 1 } else { e };
                let n = &mut self.nodes[nid as usize];
                for v in from..=e {
                    if n.conn_count as usize >= MAX_CONNS {
                        break;
                    }
                    n.conns[n.conn_count as usize] = v as u8;
                    n.conn_count += 1;
                }
                prev = e;
            }
            i += per;
        }
    }

    fn type_of(&self, nid: u32) -> u32 {
        (self.nodes[nid as usize].caps >> 20) & 0xF
    }

    /// Depth-first from `nid` towards a DAC; fills path[0..len] with the
    /// nodes and choice[] with which connection each one takes. The path
    /// length, or 0 if no DAC is reachable.
    fn find_dac(
        &self,
        nid: u32,
        depth: usize,
        path: &mut [u8; PATH_MAX_DEPTH],
        choice: &mut [u8; PATH_MAX_DEPTH],
    ) -> usize {
        path[depth] = nid as u8;
        let t = self.type_of(nid);
        let node = &self.nodes[nid as usize];
        if t == WIDGET_OUTPUT {
            return if node.caps & WCAP_DIGITAL != 0 { 0 } else { depth + 1 };
        }
        if depth + 1 >= PATH_MAX_DEPTH || (depth > 0 && t != WIDGET_MIXER && t != WIDGET_SELECTOR) {
            return 0;
        }
        for i in 0..node.conn_count as usize {
            choice[depth] = i as u8;
            let len = self.find_dac(node.conns[i] as u32, depth + 1, path, choice);
            if len > 0 {
                return len;
            }
        }
        0
    }

    /// 0 dB: the amp capability's offset field (7.3.4.10).
    fn zero_db(&mut self, nid: u32, input: bool) -> u32 {
        let caps = if self.nodes[nid as usize].caps & WCAP_AMP_OVERRIDE != 0 {
            self.param(nid, if input { PARAM_IN_AMP_CAPS } else { PARAM_OUT_AMP_CAPS })
        } else if input {
            self.in_amp_default
        } else {
            self.out_amp_default
        };
        let offset = caps & 0x7F;
        let steps = (caps >> 8) & 0x7F;
        offset.min(steps)
    }

    fn unmute_out(&mut self, nid: u32) {
        if self.nodes[nid as usize].caps & WCAP_OUT_AMP != 0 {
            let db = self.zero_db(nid, false);
            self.verb4(nid, VERB4_SET_AMP, 0xB000 | db); // Output, left + right.
        }
    }

    fn unmute_in(&mut self, nid: u32, index: u32) {
        if self.nodes[nid as usize].caps & WCAP_IN_AMP != 0 {
            let db = self.zero_db(nid, true);
            self.verb4(nid, VERB4_SET_AMP, 0x7000 | index << 8 | db);
        }
    }

    fn power_up(&mut self, nid: u32) {
        if self.nodes[nid as usize].caps & WCAP_POWER != 0 {
            self.verb(nid, VERB_SET_POWER_STATE, 0); // D0
        }
    }

    /// Routes one output pin to a DAC: selects each hop, unmutes every amp
    /// along the way, enables the pin (and its external amplifier).
    fn route_pin(&mut self, pin: u32) -> bool {
        let (mut path, mut choice) = ([0u8; PATH_MAX_DEPTH], [0u8; PATH_MAX_DEPTH]);
        let len = self.find_dac(pin, 0, &mut path, &mut choice);
        if len == 0 {
            return false;
        }
        for i in 0..len {
            let nid = path[i] as u32;
            self.power_up(nid);
            if i + 1 < len {
                if self.type_of(nid) == WIDGET_MIXER {
                    self.unmute_in(nid, choice[i] as u32);
                } else if self.nodes[nid as usize].conn_count > 1 {
                    self.verb(nid, VERB_SET_CONN_SELECT, choice[i] as u32);
                }
            }
            self.unmute_out(nid);
        }

        let device = (self.nodes[pin as usize].config >> 20) & 0xF;
        self.verb(pin, VERB_SET_PIN_CONTROL, if device == DEVICE_HEADPHONE { 0xC0 } else { 0x40 }); // Out (+HP).
        if self.nodes[pin as usize].pin_caps & PINCAP_EAPD != 0 {
            self.verb(pin, VERB_SET_EAPD, 0x02);
        }

        let dac = path[len - 1];
        if !self.dacs[..self.dac_count].contains(&dac) && self.dac_count < MAX_DACS {
            self.dacs[self.dac_count] = dac;
            self.dac_count += 1;
        }
        let name = match device {
            DEVICE_LINE_OUT => "line out",
            DEVICE_SPEAKER => "speaker",
            _ => "headphones",
        };
        kprintln!("hda: {name} (pin 0x{pin:x}) <- DAC 0x{dac:x}, {} hops", len - 1);
        true
    }

    /// Finds the codec's audio function group and routes every analog
    /// output pin it can. How many it routed.
    fn setup_codec(&mut self) -> usize {
        self.command_timeouts = 0;
        let vendor = self.param(0, PARAM_VENDOR_ID);
        let sub = self.param(0, PARAM_NODE_COUNT);
        let first = (sub >> 16) & 0xFF;
        self.afg =
            (first..first + (sub & 0xFF)).find(|&nid| self.param(nid, PARAM_FUNCTION_TYPE) & 0xFF == 1).unwrap_or(0);
        if self.afg == 0 {
            kprintln!("hda: codec {} ({:x}:{:x}): no audio function", self.codec, vendor >> 16, vendor & 0xFFFF);
            return 0;
        }
        self.verb(self.afg, VERB_SET_POWER_STATE, 0);
        timer::sleep_ms(10);
        self.out_amp_default = self.param(self.afg, PARAM_OUT_AMP_CAPS);
        self.in_amp_default = self.param(self.afg, PARAM_IN_AMP_CAPS);

        self.nodes = [NODE_EMPTY; MAX_NODES];
        let sub = self.param(self.afg, PARAM_NODE_COUNT);
        let (first, count) = ((sub >> 16) & 0xFF, sub & 0xFF);
        let nids = first..(first + count).min(MAX_NODES as u32);
        for nid in nids.clone() {
            self.nodes[nid as usize].caps = self.param(nid, PARAM_WIDGET_CAPS);
            self.read_connections(nid);
            if self.type_of(nid) == WIDGET_PIN {
                self.nodes[nid as usize].pin_caps = self.param(nid, PARAM_PIN_CAPS);
                self.nodes[nid as usize].config = self.verb(nid, VERB_GET_CONFIG_DEFAULT, 0);
            }
        }
        if self.command_timeouts > 0 {
            return 0;
        }
        kprintln!("hda: codec {}: {:x}:{:x}, {count} widgets", self.codec, vendor >> 16, vendor & 0xFFFF);

        self.dac_count = 0;
        let mut routed = 0;
        for nid in nids {
            let n = self.nodes[nid as usize];
            let device = (n.config >> 20) & 0xF;
            let usable = self.type_of(nid) == WIDGET_PIN
                && n.config >> 30 != 1 // 1: nothing attached
                && n.pin_caps & PINCAP_OUT != 0
                && n.pin_caps & (PINCAP_HDMI | PINCAP_DP) == 0
                && n.caps & WCAP_DIGITAL == 0
                && device <= DEVICE_HEADPHONE;
            if usable && self.route_pin(nid) {
                routed += 1;
            }
        }
        routed
    }

    fn try_controller(&mut self, pci: &PciDevice) -> bool {
        pci.write16(0x04, pci.read16(0x04) | 0x0406);
        // TCSEL = 0, as Linux does everywhere: avoids static on some codecs.
        pci.write8(0x44, pci.read8(0x44) & 0xF8);
        if pci.vendor_id == 0x1022 || pci.vendor_id == 0x1002 {
            // AMD/ATI: turn on cache snooping (Linux's ATI snoop type).
            pci.write8(0x42, pci.read8(0x42) & 0xF8 | 0x02);
        }

        let mut bar = (pci.bar[0] & !0xF) as u64;
        if pci.bar[0] & 0x6 == 0x4 {
            bar |= (pci.bar[1] as u64) << 32;
        }
        self.regs = Regs(vmm::map_mmio(bar, 0x1000));

        let (b, s, f) = (pci.bus, pci.slot, pci.func);
        if !self.reset_controller() {
            kprintln!("hda: {b}:{s}.{f}: controller reset timed out");
            return false;
        }
        self.wr32(INTCTL, 0); // Polled.
        let codecs = self.rd16(STATESTS);
        self.wr16(STATESTS, codecs);
        if codecs == 0 {
            kprintln!("hda: {b}:{s}.{f}: no codecs");
            return false;
        }
        if !self.start_rings() {
            return false;
        }

        let gcap = self.rd16(GCAP);
        let input_streams = ((gcap >> 8) & 0xF) as u32;
        if (gcap >> 12) & 0xF == 0 {
            kprintln!("hda: {b}:{s}.{f}: no output streams");
            return false;
        }
        self.stream_base = sd_base(input_streams); // Output streams come after the inputs.

        for codec in 0..15 {
            self.codec = codec;
            if codecs & (1 << codec) != 0 && self.setup_codec() > 0 {
                return true;
            }
        }
        kprintln!("hda: {b}:{s}.{f}: no analog outputs (HDMI/DP audio needs a GPU driver)");
        false
    }

    // --- Playback ---

    fn stop_stream(&mut self) {
        self.wr8(self.stream_base + SD_CTL, self.rd8(self.stream_base + SD_CTL) & !SD_CTL_RUN);
        self.wait8(self.stream_base + SD_CTL, SD_CTL_RUN, 0, 20);
        self.running = false;
    }

    fn start_stream(&mut self) {
        self.wr8(self.stream_base + SD_CTL, self.rd8(self.stream_base + SD_CTL) | SD_CTL_RUN);
        self.running = true;
    }

    /// Advances `played` by how far the DMA got, and zeroes what it has
    /// finished with -- so if the writer falls behind, the ring wraps into
    /// silence rather than replaying old audio.
    fn update_played(&mut self) {
        if !self.running {
            return;
        }
        let lpib = self.rd32(self.stream_base + SD_LPIB) % BUFFER_BYTES;
        let delta = (lpib + BUFFER_BYTES - self.last_lpib) % BUFFER_BYTES;
        let mut i = 0;
        while i < delta {
            let at = (self.last_lpib + i) % BUFFER_BYTES;
            let n = (BUFFER_BYTES - at).min(delta - i);
            let p = unsafe { self.buffer.add(at as usize) };
            unsafe { core::ptr::write_bytes(p, 0, n as usize) };
            clflush_range(p, n as usize);
            i += n;
        }
        self.last_lpib = lpib;
        self.played += delta as u64;
        if self.played > self.written {
            self.written = self.played; // Underrun: carry on from where it's playing now.
        }
    }

    /// Copies as much of `src` (stereo S16LE) into the ring as fits right
    /// now; how much that was.
    fn put(&mut self, src: &[u8]) -> usize {
        let queued = self.written - self.played;
        let space =
            if queued + GUARD_BYTES >= BUFFER_BYTES as u64 { 0 } else { BUFFER_BYTES as u64 - GUARD_BYTES - queued };
        let n = if src.len() as u64 > space { space as usize & !3 } else { src.len() }; // Whole stereo frames.
        let mut done = 0;
        while done < n {
            let at = (self.written % BUFFER_BYTES as u64) as usize;
            let chunk = (BUFFER_BYTES as usize - at).min(n - done);
            let p = unsafe { self.buffer.add(at) };
            unsafe { core::ptr::copy_nonoverlapping(src[done..].as_ptr(), p, chunk) };
            clflush_range(p, chunk);
            self.written += chunk as u64;
            done += chunk;
        }
        n
    }

    fn open(&mut self, rate_hz: u32, channels: u8) -> bool {
        if !self.ready || (rate_hz != 44100 && rate_hz != 48000) || (channels != 1 && channels != 2) {
            return false;
        }
        if self.open {
            self.stop_stream();
        }
        let sb = self.stream_base;

        // Stream reset (3.3.35).
        self.wr8(sb + SD_CTL, SD_CTL_SRST);
        self.wait8(sb + SD_CTL, SD_CTL_SRST, SD_CTL_SRST, 20);
        self.wr8(sb + SD_CTL, 0);
        self.wait8(sb + SD_CTL, SD_CTL_SRST, 0, 20);

        // PCM, 44.1 or 48 kHz base, x1, /1, 16-bit, 2 channels (3.7.1).
        let fmt: u16 = if rate_hz == 44100 { 0x4000 } else { 0 } | 1 << 4 | 1;
        unsafe { core::ptr::write_bytes(self.buffer, 0, BUFFER_BYTES as usize) };
        clflush_range(self.buffer, BUFFER_BYTES as usize);
        self.wr32(sb + SD_BDPL, self.bdl_phys as u32);
        self.wr32(sb + SD_BDPU, (self.bdl_phys >> 32) as u32);
        self.wr32(sb + SD_CBL, BUFFER_BYTES);
        self.wr16(sb + SD_LVI, BDL_ENTRIES as u16 - 1);
        self.wr16(sb + SD_FMT, fmt);
        self.wr8(sb + SD_CTL + 2, STREAM_TAG << 4);
        self.wr8(sb + SD_STS, 0x1C); // Clear any stale status.

        for i in 0..self.dac_count {
            let dac = self.dacs[i] as u32;
            self.verb4(dac, VERB4_SET_FORMAT, fmt as u32);
            self.verb(dac, VERB_SET_STREAM_CHANNEL, (STREAM_TAG << 4) as u32); // Channels 0-1.
        }

        self.mono = channels == 1;
        (self.written, self.played, self.last_lpib) = (0, 0, 0);
        self.running = false;
        self.open = true;
        true
    }

    /// Copies PCM into the DMA ring, blocking while it's full -- which
    /// paces the caller to real time. False if the stream isn't open or
    /// the hardware stopped moving.
    fn write(&mut self, pcm: &[u8]) -> bool {
        if !self.open {
            return false;
        }
        let mut stereo = [0u8; 1024];
        let mut stalled_since = timer::uptime_ms();
        let mut last_played = self.played;

        let mut done = 0;
        while done < pcm.len() {
            // Mono goes out on both channels, a slice at a time.
            let rest = &pcm[done..];
            let (in_len, chunk): (usize, &[u8]) = if self.mono {
                let in_len = if rest.len() > stereo.len() / 2 { stereo.len() / 2 } else { rest.len() & !1 };
                if in_len == 0 {
                    break; // A stray odd byte.
                }
                for i in (0..in_len).step_by(2) {
                    stereo[i * 2..i * 2 + 4].copy_from_slice(&[rest[i], rest[i + 1], rest[i], rest[i + 1]]);
                }
                (in_len, &stereo[..in_len * 2])
            } else {
                (rest.len(), rest)
            };

            let mut out_done = 0;
            while out_done < chunk.len() {
                self.update_played();
                let n = self.put(&chunk[out_done..]);
                out_done += n;
                if !self.running && self.written - self.played >= BUFFER_BYTES as u64 / 2 {
                    self.start_stream();
                }
                if n == 0 {
                    if !self.running {
                        self.start_stream();
                    }
                    if self.played != last_played {
                        last_played = self.played;
                        stalled_since = timer::uptime_ms();
                    } else if timer::uptime_ms() - stalled_since > 1000 {
                        kprintln!("hda: playback stalled -- the stream isn't moving");
                        self.stop_stream();
                        self.open = false;
                        return false;
                    }
                    // Lets the tick in, so the stall timeout keeps counting
                    // inside the syscall.
                    timer::idle();
                }
            }
            done += in_len;
        }
        true
    }

    /// Lets what's queued finish playing, then stops the stream.
    fn close(&mut self) -> bool {
        if !self.open {
            return false;
        }
        // 50 ms of silence after the end: stopping the stream the moment the
        // last byte leaves would cut off whatever the codec still holds.
        let silence = [0u8; 1024];
        let pad = 48000 / 20 * 4;
        let mut done = 0;
        while done < pad && self.open {
            let n = (pad - done).min(silence.len());
            if !self.write(&silence[..if self.mono { n / 2 } else { n }]) {
                return false;
            }
            done += n;
        }
        if !self.running && self.written > self.played {
            self.start_stream(); // Shorter than half the buffer: never got going.
        }
        let deadline = timer::uptime_ms() + BUFFER_BYTES as u64 * 1000 / (44100 * 4) + 200;
        while self.running && self.played < self.written && timer::uptime_ms() < deadline {
            self.update_played();
            timer::idle();
        }
        self.stop_stream();
        self.open = false;
        true
    }
}

/// Probes every HD Audio controller (PCI class 04.03) and keeps the first
/// whose codec has an analog output it can route. pci_enumerate() and the
/// timer must be up. True if there's somewhere to play.
pub fn init() -> bool {
    let h = hda();
    for i in 0.. {
        let Some(pci) = pci::find_class_nth(0x04, 0x03, 0x00, i) else { break };
        kprintln!("hda: controller at {}:{}.{} ({:x}:{:x})", pci.bus, pci.slot, pci.func, pci.vendor_id, pci.device_id);
        if !h.try_controller(pci) {
            if !h.regs.0.is_null() {
                h.wr8(CORBCTL, 0); // Leave it quiet: no ring DMA left running.
                h.wr8(RIRBCTL, 0);
                h.regs = Regs(core::ptr::null_mut());
            }
            continue;
        }

        let (Some((buffer, buffer_phys)), Some((bdl, bdl_phys))) =
            (dma_pages(BUFFER_BYTES as u64 / PAGE_SIZE), dma_pages(1))
        else {
            kprintln!("hda: out of memory");
            return false;
        };
        (h.buffer, h.buffer_phys, h.bdl, h.bdl_phys) = (buffer, buffer_phys, bdl.cast(), bdl_phys);
        let per_entry = BUFFER_BYTES / BDL_ENTRIES;
        for e in 0..BDL_ENTRIES {
            let entry = BdlEntry { address: buffer_phys + (e * per_entry) as u64, length: per_entry, flags: 0 }; // No completion interrupts: polled.
            unsafe { h.bdl.add(e as usize).write_volatile(entry) };
        }
        clflush_range(bdl, size_of::<BdlEntry>() * BDL_ENTRIES as usize);
        h.ready = true;
        return true;
    }
    false
}

pub fn open(rate_hz: u32, channels: u8) -> bool {
    hda().open(rate_hz, channels)
}

pub fn write(pcm: &[u8]) -> bool {
    hda().write(pcm)
}

pub fn close() -> bool {
    hda().close()
}
