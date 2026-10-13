//! The self-test payloads: small programs that each prove one piece of
//! the ring-3 pipeline works -- syscalls, faults, the heap, files,
//! directories, fork/exec/wait, preemption, raw mode, pipes. The kernel
//! puts them all in /bin at boot (kernel/src/main.rs); run one by name
//! from the shell.
//!
//! They're one crate rather than one per test: each module exports
//! `<name>_main`, and scripts/build-userland.sh links the staticlib once
//! per test with `main` pointed at that test's entry point.

#![no_std]

extern crate alloc;

mod crash;
mod dirtest;
mod filetest;
mod forkchild;
mod forktest;
mod hello;
mod malloctest;
mod pipetest;
mod preempttest;
mod readdirtest;
mod termtest;

use core::ffi::c_char;

/// Every test's entry point takes what `_start` hands `main`.
type Argv = *const *const c_char;

/// Bytes read back from a file or pipe, as text.
fn text(buf: &[u8]) -> &str {
    core::str::from_utf8(buf).unwrap_or("(not UTF-8)")
}
