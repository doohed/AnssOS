//! virtio-sound playback: the control and tx virtqueues only (the
//! event/rx queues go untouched -- this driver only ever plays audio, the
//! same "only wire up the queues you use" precedent gpu.rs sets by
//! skipping its cursor queue), on stream 0, which must be an output.
//!
//! The protocol structs and constants match Linux's real
//! include/uapi/linux/virtio_snd.h (also the virtio-v1.2 spec, "Sound
//! Device") -- read in full rather than guessed, same "reuse the real
//! values" reasoning docs/syscalls.md gives for Linux syscall numbers.

use alloc::boxed::Box;
use alloc::vec;
use alloc::vec::Vec;

use super::{Buffer, VirtioDevice, Virtqueue};
use crate::drivers::pci;
use crate::sync::Global;

const PCI_VENDOR_ID: u16 = 0x1af4;
const PCI_DEVICE_ID: u16 = 0x1059;

const VQ_CONTROL: u16 = 0;
const VQ_TX: u16 = 2;
const D_OUTPUT: u8 = 0;

const R_PCM_INFO: u32 = 0x0100;
const R_PCM_SET_PARAMS: u32 = 0x0101;
const R_PCM_PREPARE: u32 = 0x0102;
const R_PCM_RELEASE: u32 = 0x0103;
const R_PCM_START: u32 = 0x0104;
const R_PCM_STOP: u32 = 0x0105;
const S_OK: u32 = 0x8000;

const PCM_FMT_S16: u8 = 5;
const PCM_RATE_44100: u8 = 6;
const PCM_RATE_48000: u8 = 7;

const PERIOD_BYTES: usize = 4096;
const BUFFER_BYTES: u32 = 8 * PERIOD_BYTES as u32;

#[repr(C, packed)]
struct Config {
    jacks: u32,
    streams: u32,
    chmaps: u32,
    controls: u32,
}

#[repr(C, packed)]
struct QueryInfo {
    code: u32,
    start_id: u32,
    count: u32,
    size: u32,
}

#[repr(C, packed)]
#[derive(Default)]
struct PcmInfo {
    hda_fn_nid: u32,
    features: u32,
    formats: u64,
    rates: u64,
    direction: u8,
    channels_min: u8,
    channels_max: u8,
    padding: [u8; 5],
}

/// Per the spec (5.14.6.2, "Driver Requirements: Item Information
/// Request"): the response to an *_INFO query is a status header followed
/// by count * size bytes, not just the raw item. Getting this wrong
/// doesn't crash anything (QEMU just rejects the request as BAD_MSG and
/// never writes the payload), but then reading `info` reads uninitialized
/// memory instead of a real answer.
#[repr(C, packed)]
#[derive(Default)]
struct PcmInfoResp {
    code: u32,
    info: PcmInfo,
}

/// SET_PARAMS uses every field; PREPARE/START/STOP/RELEASE only look at
/// the leading code and stream id, so sending the same (mostly zeroed)
/// struct for all of them is harmless -- the device only reads as many
/// bytes as the request needs.
#[repr(C, packed)]
#[derive(Default)]
struct PcmSetParams {
    code: u32,
    stream_id: u32,
    buffer_bytes: u32,
    period_bytes: u32,
    features: u32,
    channels: u8,
    format: u8,
    rate: u8,
    padding: u8,
}

#[repr(C, packed)]
struct PcmXfer {
    stream_id: u32,
}

#[repr(C, packed)]
struct PcmStatus {
    status: u32,
    latency_bytes: u32,
}

struct Snd {
    controlq: Virtqueue,
    txq: Virtqueue,
    stream_open: bool,
    // Request and response buffers, on the heap (HHDM memory the device
    // can DMA) and reused: requests here are synchronous, one at a time.
    params: Box<PcmSetParams>,
    ctrl_code: Box<u32>,
    tx_hdr: Box<PcmXfer>,
    tx_status: Box<PcmStatus>,
    /// audio_write()'s caller hands over a *userland* pointer, in a
    /// different per-process address space with no relationship to the
    /// HHDM offset the device's physical address is derived with. DMAing
    /// from it directly reads whatever physical page that arithmetic lands
    /// on -- wrong but not a crash, which is what made this easy to miss
    /// until the captured audio came back silent. Every tx chunk is copied
    /// through this HHDM-backed buffer first.
    tx_bounce: Vec<u8>,
}

static SND: Global<Option<Snd>> = Global::new(None);

fn snd() -> Option<&'static mut Snd> {
    unsafe { SND.get() }.as_mut()
}

impl Snd {
    fn pcm_ctrl(&mut self, code: u32, channels: u8, format: u8, rate: u8) -> bool {
        let (buffer_bytes, period_bytes) =
            if code == R_PCM_SET_PARAMS { (BUFFER_BYTES, PERIOD_BYTES as u32) } else { (0, 0) };
        *self.params = PcmSetParams {
            code,
            stream_id: 0,
            buffer_bytes,
            period_bytes,
            channels,
            format,
            rate,
            ..Default::default()
        };
        *self.ctrl_code = 0xFFFF_FFFF;
        let req: *const PcmSetParams = &*self.params;
        let resp: *mut u32 = &mut *self.ctrl_code;
        self.controlq.transfer(&[Buffer::to_device(req, size_of::<PcmSetParams>()), Buffer::from_device(resp, 4)]);
        unsafe { resp.read_volatile() == S_OK }
    }

    fn stream0_is_output(&mut self) -> bool {
        let req = Box::new(QueryInfo { code: R_PCM_INFO, start_id: 0, count: 1, size: size_of::<PcmInfo>() as u32 });
        let mut resp = Box::new(PcmInfoResp { code: 0xFFFF_FFFF, ..Default::default() });
        let resp_ptr: *mut PcmInfoResp = &mut *resp;
        self.controlq.transfer(&[
            Buffer::to_device(&*req, size_of::<QueryInfo>()),
            Buffer::from_device(resp_ptr, size_of::<PcmInfoResp>()),
        ]);
        let resp = unsafe { resp_ptr.read_volatile() };
        let (code, direction) = (resp.code, resp.info.direction);
        code == S_OK && direction == D_OUTPUT
    }
}

/// Finds and starts the device (pci_enumerate() must have run). False if
/// there's none -- non-fatal, audio is just skipped.
pub fn init() -> bool {
    let Some(pci) = pci::find_device(PCI_VENDOR_ID, PCI_DEVICE_ID) else {
        kprintln!("virtio-snd: no sound device found -- boot QEMU with -device virtio-sound-pci");
        return false;
    };
    let Some(dev) = VirtioDevice::new(pci).filter(|d| d.negotiate_features(0)) else { return false };
    let (Some(controlq), Some(txq)) = (dev.queue(VQ_CONTROL), dev.queue(VQ_TX)) else { return false };
    dev.driver_ok();

    let cfg = dev.device_cfg.cast::<Config>();
    let streams = vread!(cfg, streams);
    kprintln!("virtio-snd: {} jack(s), {streams} stream(s), {} chmap(s)", vread!(cfg, jacks), vread!(cfg, chmaps));

    let mut s = Snd {
        controlq,
        txq,
        stream_open: false,
        params: Box::default(),
        ctrl_code: Box::new(0),
        tx_hdr: Box::new(PcmXfer { stream_id: 0 }),
        tx_status: Box::new(PcmStatus { status: 0, latency_bytes: 0 }),
        tx_bounce: vec![0; PERIOD_BYTES],
    };
    if streams == 0 || !s.stream0_is_output() {
        kprintln!("virtio-snd: stream 0 is not an output stream -- no playback available");
        return false;
    }
    unsafe { *SND.get() = Some(s) };
    kprintln!("virtio-snd: ready");
    true
}

/// Configures stream 0 (SET_PARAMS + PREPARE + START). S16LE only;
/// `rate_hz` must be 44100 or 48000 and `channels` 1 or 2.
pub fn open(rate_hz: u32, channels: u8) -> bool {
    let Some(s) = snd() else { return false };
    let rate = match rate_hz {
        44100 => PCM_RATE_44100,
        48000 => PCM_RATE_48000,
        _ => return false,
    };
    if channels != 1 && channels != 2 {
        return false;
    }
    for (code, what) in [(R_PCM_SET_PARAMS, "SET_PARAMS"), (R_PCM_PREPARE, "PREPARE"), (R_PCM_START, "START")] {
        if !s.pcm_ctrl(code, channels, PCM_FMT_S16, rate) {
            kprintln!("virtio-snd: PCM_{what} failed");
            return false;
        }
    }
    s.stream_open = true;
    true
}

/// Sends `pcm` to stream 0 a period at a time, blocking until each is
/// consumed -- which paces playback to real time. Bytes sent, or None.
pub fn write(pcm: &[u8]) -> Option<usize> {
    let s = snd().filter(|s| s.stream_open)?;
    let mut sent = 0;
    for chunk in pcm.chunks(PERIOD_BYTES) {
        s.tx_hdr.stream_id = 0;
        s.tx_status.status = 0xFFFF_FFFF;
        s.tx_bounce[..chunk.len()].copy_from_slice(chunk);
        let hdr: *const PcmXfer = &*s.tx_hdr;
        let status: *mut PcmStatus = &mut *s.tx_status;
        s.txq.transfer(&[
            Buffer::to_device(hdr, size_of::<PcmXfer>()),
            Buffer::to_device(s.tx_bounce.as_ptr(), chunk.len()),
            Buffer::from_device(status, size_of::<PcmStatus>()),
        ]);
        if vread!(status, status) != S_OK {
            return if sent > 0 { Some(sent) } else { None };
        }
        sent += chunk.len();
    }
    Some(sent)
}

/// STOP + RELEASE on stream 0.
pub fn close() -> bool {
    let Some(s) = snd().filter(|s| s.stream_open) else { return false };
    let stopped = s.pcm_ctrl(R_PCM_STOP, 0, 0, 0);
    let released = s.pcm_ctrl(R_PCM_RELEASE, 0, 0, 0);
    s.stream_open = false;
    stopped && released
}
