//! M10 happy-path test payload: proves write(), read(), and a clean
//! exit() all round-trip correctly through AnssOS's ring-3 pipeline.

use core::ffi::c_int;

use anssos::sys;

use crate::Argv;

#[unsafe(no_mangle)]
pub extern "C" fn hello_main(_argc: c_int, _argv: Argv) -> c_int {
    anssos::write_all(1, b"Hello from ring 3! AnssOS userspace is alive.\nType something: ");

    let mut buf = [0u8; 64];
    let n = unsafe { sys::read(0, buf.as_mut_ptr(), buf.len()) }.max(0) as usize;

    anssos::write_all(1, b"You typed: ");
    anssos::write_all(1, &buf[..n]);
    anssos::write_all(1, b"\n");
    42
}
