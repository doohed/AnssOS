//! Test payload: proves getdents() reaches the real VFS tree -- lists "/"
//! and prints each entry's name and type. A follow-up `ls /` from the
//! shell confirms the listing matches.

use core::ffi::c_int;

use anssos::println;

use crate::Argv;

#[unsafe(no_mangle)]
pub extern "C" fn readdirtest_main(_argc: c_int, _argv: Argv) -> c_int {
    let Some(entries) = anssos::read_dir(c"/") else {
        println!("opendir(/) failed");
        return 1;
    };
    for e in &entries {
        println!("{} {}", if e.is_dir { "dir " } else { "file" }, e.name);
    }
    println!("readdirtest done: {} entries", entries.len());
    0
}
