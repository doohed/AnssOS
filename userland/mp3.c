/* The one translation unit that compiles minimp3's implementation (a
 * CC0 single-header MP3 decoder, vendored unmodified in
 * third_party/minimp3/) -- play.c includes only its declarations.
 * minimp3 is float-based; that works only because userland now builds
 * with SSE enabled and the kernel saves/restores per-process FPU state
 * (kernel/src/arch/x86_64/fpu.h). */
#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3/minimp3.h"
