//! virtio-gpu: finds the device, completes the virtio init handshake,
//! creates a 2D resource sized to the display's preferred mode (falling
//! back to 1024x768), attaches a PMM-backed linear buffer to it, and sets
//! it as scanout 0. Normally reached through drivers/display.rs.

use super::{Buffer, VirtioDevice, Virtqueue};
use crate::boot::requests::phys_to_virt;
use crate::drivers::display::Framebuffer;
use crate::drivers::pci;
use crate::mm::pmm::{self, PAGE_SIZE};
use crate::sync::Global;

pub const PCI_VENDOR_ID: u16 = 0x1af4;
pub const PCI_DEVICE_ID: u16 = 0x1050;

const CMD_GET_DISPLAY_INFO: u32 = 0x0100;
const CMD_RESOURCE_CREATE_2D: u32 = 0x0101;
const CMD_SET_SCANOUT: u32 = 0x0103;
const CMD_RESOURCE_FLUSH: u32 = 0x0104;
const CMD_TRANSFER_TO_HOST_2D: u32 = 0x0105;
const CMD_RESOURCE_ATTACH_BACKING: u32 = 0x0106;

const RESP_OK_NODATA: u32 = 0x1100;
const RESP_OK_DISPLAY_INFO: u32 = 0x1101;

const FORMAT_B8G8R8X8_UNORM: u32 = 2;
const MAX_SCANOUTS: usize = 16;
const RESOURCE_ID: u32 = 1;

const DEFAULT_WIDTH: u32 = 1024;
const DEFAULT_HEIGHT: u32 = 768;
const MAX_WIDTH: u32 = 1920;
const MAX_HEIGHT: u32 = 1080;

#[repr(C, packed)]
#[derive(Default, Clone, Copy)]
struct CtrlHdr {
    kind: u32,
    flags: u32,
    fence_id: u64,
    ctx_id: u32,
    ring_idx: u8,
    padding: [u8; 3],
}

impl CtrlHdr {
    fn new(kind: u32) -> CtrlHdr {
        CtrlHdr { kind, ..Default::default() }
    }
}

#[repr(C, packed)]
#[derive(Default, Clone, Copy)]
struct Rect {
    x: u32,
    y: u32,
    width: u32,
    height: u32,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct DisplayOne {
    r: Rect,
    enabled: u32,
    flags: u32,
}

#[repr(C, packed)]
struct RespDisplayInfo {
    hdr: CtrlHdr,
    pmodes: [DisplayOne; MAX_SCANOUTS],
}

#[repr(C, packed)]
struct ResourceCreate2d {
    hdr: CtrlHdr,
    resource_id: u32,
    format: u32,
    width: u32,
    height: u32,
}

#[repr(C, packed)]
struct ResourceAttachBacking {
    hdr: CtrlHdr,
    resource_id: u32,
    nr_entries: u32,
    addr: u64,
    length: u32,
    padding: u32,
}

#[repr(C, packed)]
struct SetScanout {
    hdr: CtrlHdr,
    r: Rect,
    scanout_id: u32,
    resource_id: u32,
}

#[repr(C, packed)]
struct TransferToHost2d {
    hdr: CtrlHdr,
    r: Rect,
    offset: u64,
    resource_id: u32,
    padding: u32,
}

#[repr(C, packed)]
struct ResourceFlush {
    hdr: CtrlHdr,
    r: Rect,
    resource_id: u32,
    padding: u32,
}

struct Gpu {
    controlq: Virtqueue,
    /// Scratch request/response pages for every command: virtqueue
    /// descriptors carry physical addresses, so these come from the PMM.
    req: *mut u8,
    resp: *mut u8,
    width: u32,
    height: u32,
}

static GPU: Global<Option<Gpu>> = Global::new(None);

impl Gpu {
    /// Every control command has the same request/response shape: copy
    /// into the scratch pages, submit as a 2-descriptor chain (request
    /// readable, response writable), and poll for completion.
    fn cmd<Req, Resp>(&mut self, req: &Req) -> Resp {
        unsafe {
            core::ptr::copy_nonoverlapping((req as *const Req).cast::<u8>(), self.req, size_of::<Req>());
            core::ptr::write_bytes(self.resp, 0, size_of::<Resp>());
        }
        self.controlq.transfer(&[
            Buffer::to_device(self.req, size_of::<Req>()),
            Buffer::from_device(self.resp, size_of::<Resp>()),
        ]);
        unsafe { self.resp.cast::<Resp>().read_unaligned() }
    }

    fn display_size(&mut self) -> Option<(u32, u32)> {
        let resp: RespDisplayInfo = self.cmd(&CtrlHdr::new(CMD_GET_DISPLAY_INFO));
        let kind = resp.hdr.kind;
        if kind != RESP_OK_DISPLAY_INFO {
            kprintln!("virtio-gpu: GET_DISPLAY_INFO failed (resp type 0x{kind:x})");
            return None;
        }
        let mode = resp.pmodes[0];
        let (enabled, width, height) = (mode.enabled, mode.r.width, mode.r.height);
        if enabled == 0 || width == 0 || height == 0 {
            return None;
        }
        Some((width.min(MAX_WIDTH), height.min(MAX_HEIGHT)))
    }

    /// Sends a command that answers with a bare header; false (and a log
    /// line naming `what`) unless it's OK_NODATA.
    fn simple_cmd<Req>(&mut self, req: &Req, what: &str) -> bool {
        let resp: CtrlHdr = self.cmd(req);
        let kind = resp.kind;
        if kind != RESP_OK_NODATA {
            kprintln!("virtio-gpu: {what} failed (0x{kind:x})");
        }
        kind == RESP_OK_NODATA
    }
}

/// Brings the GPU up; the framebuffer to draw into (HHDM-mapped,
/// pitch == width). pci::pci_enumerate() must have run.
pub fn init() -> Option<Framebuffer> {
    let Some(pci) = pci::find_device(PCI_VENDOR_ID, PCI_DEVICE_ID) else {
        kprintln!("virtio-gpu: device not found");
        return None;
    };
    let dev = VirtioDevice::new(pci)?;
    if !dev.negotiate_features(0) {
        return None;
    }
    let controlq = dev.queue(0)?;
    dev.driver_ok();
    kprintln!("virtio-gpu: init handshake complete (ACKNOWLEDGE/DRIVER/FEATURES_OK/DRIVER_OK)");

    let (req, resp) = (pmm::alloc_page(), pmm::alloc_page());
    if req == 0 || resp == 0 {
        kprintln!("virtio-gpu: out of memory allocating command scratch buffers");
        return None;
    }
    let mut gpu = Gpu { controlq, req: phys_to_virt(req), resp: phys_to_virt(resp), width: 0, height: 0 };

    let (width, height) = match gpu.display_size() {
        Some((w, h)) => {
            kprintln!("virtio-gpu: display reports {w}x{h}");
            (w, h)
        }
        None => {
            kprintln!("virtio-gpu: no usable display info, defaulting to {DEFAULT_WIDTH}x{DEFAULT_HEIGHT}");
            (DEFAULT_WIDTH, DEFAULT_HEIGHT)
        }
    };
    (gpu.width, gpu.height) = (width, height);

    let create = ResourceCreate2d {
        hdr: CtrlHdr::new(CMD_RESOURCE_CREATE_2D),
        resource_id: RESOURCE_ID,
        format: FORMAT_B8G8R8X8_UNORM,
        width,
        height,
    };
    if !gpu.simple_cmd(&create, "RESOURCE_CREATE_2D") {
        return None;
    }

    let pages = (width as u64 * height as u64 * 4).div_ceil(PAGE_SIZE);
    let fb_phys = pmm::alloc_pages(pages);
    if fb_phys == 0 {
        kprintln!("virtio-gpu: out of memory allocating {pages}-page framebuffer");
        return None;
    }
    let pixels: *mut u32 = phys_to_virt(fb_phys);
    unsafe { core::ptr::write_bytes(pixels.cast::<u8>(), 0, (pages * PAGE_SIZE) as usize) };

    let attach = ResourceAttachBacking {
        hdr: CtrlHdr::new(CMD_RESOURCE_ATTACH_BACKING),
        resource_id: RESOURCE_ID,
        nr_entries: 1,
        addr: fb_phys,
        length: (pages * PAGE_SIZE) as u32,
        padding: 0,
    };
    if !gpu.simple_cmd(&attach, "RESOURCE_ATTACH_BACKING") {
        return None;
    }

    let scanout = SetScanout {
        hdr: CtrlHdr::new(CMD_SET_SCANOUT),
        r: Rect { x: 0, y: 0, width, height },
        scanout_id: 0,
        resource_id: RESOURCE_ID,
    };
    if !gpu.simple_cmd(&scanout, "SET_SCANOUT") {
        return None;
    }
    kprintln!("virtio-gpu: resource {RESOURCE_ID} ({width}x{height}) created, backed, and set as scanout 0");

    unsafe { *GPU.get() = Some(gpu) };
    Some(Framebuffer { pixels, width, height, pitch: width })
}

/// Pushes the whole framebuffer to the host display: TRANSFER_TO_HOST_2D
/// then RESOURCE_FLUSH, both covering the full resource.
pub fn flush() {
    let Some(gpu) = (unsafe { GPU.get() }) else { return };
    let r = Rect { x: 0, y: 0, width: gpu.width, height: gpu.height };
    let _: CtrlHdr = gpu.cmd(&TransferToHost2d {
        hdr: CtrlHdr::new(CMD_TRANSFER_TO_HOST_2D),
        r,
        offset: 0,
        resource_id: RESOURCE_ID,
        padding: 0,
    });
    let _: CtrlHdr =
        gpu.cmd(&ResourceFlush { hdr: CtrlHdr::new(CMD_RESOURCE_FLUSH), r, resource_id: RESOURCE_ID, padding: 0 });
}
