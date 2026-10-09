/* The one translation unit that compiles minimp3's implementation (a
 * CC0 single-header MP3 decoder, vendored unmodified in
 * ../vendor/minimp3/) -- play's Rust side calls it over FFI, see
 * ../src/source.rs. Compiled by scripts/build-userland.sh with the same
 * C flags as the rest of userland.
 * minimp3 is float-based; that works only because userland builds with
 * SSE enabled and the kernel saves/restores per-process FPU state
 * (kernel/src/arch/x86_64/fpu.h). */
#define MINIMP3_IMPLEMENTATION
#include "../vendor/minimp3/minimp3.h"
