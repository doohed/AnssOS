//! Everything the kernel asks of the Limine bootloader, gathered in one
//! place. See PROTOCOL.md (vendored in limine/, or upstream at
//! limine-bootloader/limine-protocol) for what each request means; the
//! structs are the protocol's own layouts.
//!
//! Every request (and the base revision marker) lives in the
//! `.limine_requests` section, bracketed by the start/end markers, kept
//! alive with `#[used]` since nothing references most of them -- the
//! bootloader finds them by scanning that section (see kernel/linker.ld
//! for where it's placed) and fills in each `response` before handing
//! over control.

use core::ffi::{CStr, c_char, c_void};
use core::ptr::{null_mut, read_volatile};

const COMMON_MAGIC: [u64; 2] = [0xc7b1dd30df4c8b88, 0x0a82e883a194f07b];

const fn request_id(a: u64, b: u64) -> [u64; 4] {
    [COMMON_MAGIC[0], COMMON_MAGIC[1], a, b]
}

#[repr(C)]
pub struct Request<R> {
    id: [u64; 4],
    revision: u64,
    response: *mut R,
}

impl<R> Request<R> {
    const fn new(id: [u64; 4]) -> Request<R> {
        Request { id, revision: 0, response: null_mut() }
    }
}

#[repr(C)]
pub struct HhdmResponse {
    pub revision: u64,
    pub offset: u64,
}

pub const MEMMAP_USABLE: u64 = 0;
pub const MEMMAP_BOOTLOADER_RECLAIMABLE: u64 = 5;

#[repr(C)]
pub struct MemmapEntry {
    pub base: u64,
    pub length: u64,
    pub kind: u64,
}

#[repr(C)]
pub struct MemmapResponse {
    pub revision: u64,
    pub entry_count: u64,
    pub entries: *const *const MemmapEntry,
}

impl MemmapResponse {
    pub fn entries(&self) -> impl Iterator<Item = &MemmapEntry> {
        (0..self.entry_count as usize).map(|i| unsafe { &**self.entries.add(i) })
    }
}

#[repr(C)]
pub struct Framebuffer {
    pub address: *mut c_void,
    pub width: u64,
    pub height: u64,
    pub pitch: u64,
    pub bpp: u16,
    pub memory_model: u8,
    pub red_mask_size: u8,
    pub red_mask_shift: u8,
    pub green_mask_size: u8,
    pub green_mask_shift: u8,
    pub blue_mask_size: u8,
    pub blue_mask_shift: u8,
    pub unused: [u8; 7],
    pub edid_size: u64,
    pub edid: *mut c_void,
    pub mode_count: u64,
    pub modes: *mut c_void,
}

#[repr(C)]
pub struct FramebufferResponse {
    pub revision: u64,
    pub framebuffer_count: u64,
    pub framebuffers: *const *const Framebuffer,
}

#[repr(C)]
pub struct RsdpResponse {
    pub revision: u64,
    pub address: *mut c_void,
}

#[repr(C)]
pub struct CmdlineResponse {
    pub revision: u64,
    pub cmdline: *const c_char,
}

#[used]
#[unsafe(link_section = ".limine_requests")]
static mut BASE_REVISION: [u64; 3] = [0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, 6];

#[used]
#[unsafe(link_section = ".limine_requests")]
static mut HHDM_REQUEST: Request<HhdmResponse> = Request::new(request_id(0x48dcf1cb8ad2b852, 0x63984e959a98244b));

#[used]
#[unsafe(link_section = ".limine_requests")]
static mut MEMMAP_REQUEST: Request<MemmapResponse> = Request::new(request_id(0x67cf3d9d378a806f, 0xe304acdfc50c3c62));

#[used]
#[unsafe(link_section = ".limine_requests")]
static mut FRAMEBUFFER_REQUEST: Request<FramebufferResponse> =
    Request::new(request_id(0x9d5827dcd881dd75, 0xa3148604f6fab11b));

#[used]
#[unsafe(link_section = ".limine_requests")]
static mut RSDP_REQUEST: Request<RsdpResponse> = Request::new(request_id(0xc5e77b6b397e7b43, 0x27637845accdcf3c));

#[used]
#[unsafe(link_section = ".limine_requests")]
static mut CMDLINE_REQUEST: Request<CmdlineResponse> = Request::new(request_id(0x4b161536e598651e, 0xb390ad4a2f1f303a));

#[used]
#[unsafe(link_section = ".limine_requests_start")]
static REQUESTS_START_MARKER: [u64; 4] =
    [0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf, 0x785c6ed015d3e316, 0x181e920a7852b9d9];

#[used]
#[unsafe(link_section = ".limine_requests_end")]
static REQUESTS_END_MARKER: [u64; 2] = [0xadc0e0531bb10d03, 0x9572709f31764c62];

/// A request's response, if the bootloader answered it. Read volatile:
/// the bootloader wrote it, not anything the compiler can see.
fn response<R>(request: *const Request<R>) -> Option<&'static R> {
    unsafe { read_volatile(&raw const (*request).response).as_ref() }
}

/// Whether the bootloader speaks the protocol revision asked for above:
/// it zeroes the third word when it does.
pub fn base_revision_supported() -> bool {
    unsafe { read_volatile(&raw const BASE_REVISION[2]) == 0 }
}

pub fn hhdm() -> Option<&'static HhdmResponse> {
    response(&raw const HHDM_REQUEST)
}

pub fn memmap() -> Option<&'static MemmapResponse> {
    response(&raw const MEMMAP_REQUEST)
}

pub fn framebuffer() -> Option<&'static FramebufferResponse> {
    response(&raw const FRAMEBUFFER_REQUEST)
}

pub fn rsdp() -> Option<&'static RsdpResponse> {
    response(&raw const RSDP_REQUEST)
}

/// Where the higher-half direct map starts: physical address `p` is
/// mapped at `p + hhdm_offset()`. Every boot gets one -- the PMM refuses
/// to start without it -- so this is only ever called once it's known.
pub fn hhdm_offset() -> u64 {
    hhdm().expect("no HHDM response from the bootloader").offset
}

/// Physical address -> pointer through the higher-half direct map.
pub fn phys_to_virt<T>(phys: u64) -> *mut T {
    (phys + hhdm_offset()) as *mut T
}

/// Whether `word` is one of the space-separated words of the kernel
/// command line (limine.conf's `cmdline:`).
pub fn option(word: &str) -> bool {
    let Some(cmdline) = response(&raw const CMDLINE_REQUEST) else { return false };
    if cmdline.cmdline.is_null() {
        return false;
    }
    let line = unsafe { CStr::from_ptr(cmdline.cmdline) }.to_bytes();
    line.split(|&b| b == b' ').any(|w| w == word.as_bytes())
}
