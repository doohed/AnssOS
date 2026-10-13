//! Sound output, whichever device there is: virtio-sound under QEMU
//! (drivers/virtio/snd.rs), Intel HD Audio on a real PC (drivers/hda.rs).
//! Both take the same thing: S16LE PCM at 44100 or 48000 Hz, 1 or 2
//! channels, with write() blocking while the device's buffer is full --
//! which is what paces a player to real time.

use super::hda;
use super::virtio::snd;
use crate::sync::Global;

#[derive(Clone, Copy, PartialEq)]
enum Device {
    None,
    Virtio,
    Hda,
}

static DEVICE: Global<Device> = Global::new(Device::None);

fn device() -> Device {
    unsafe { *DEVICE.get() }
}

/// Picks the device. True if there's one.
pub fn init() -> bool {
    let d = if snd::init() {
        Device::Virtio
    } else if hda::init() {
        Device::Hda
    } else {
        return false;
    };
    unsafe { *DEVICE.get() = d };
    true
}

pub fn name() -> Option<&'static str> {
    match device() {
        Device::Virtio => Some("virtio-sound"),
        Device::Hda => Some("HD Audio"),
        Device::None => None,
    }
}

pub fn open(rate_hz: u32, channels: u8) -> bool {
    match device() {
        Device::Virtio => snd::open(rate_hz, channels),
        Device::Hda => hda::open(rate_hz, channels),
        Device::None => false,
    }
}

/// Bytes accepted, or None.
pub fn write(pcm: &[u8]) -> Option<usize> {
    match device() {
        Device::Virtio => snd::write(pcm),
        Device::Hda => hda::write(pcm).then_some(pcm.len()),
        Device::None => None,
    }
}

pub fn close() -> bool {
    match device() {
        Device::Virtio => snd::close(),
        Device::Hda => hda::close(),
        Device::None => false,
    }
}
