#ifndef ARCH_X86_64_FPU_H
#define ARCH_X86_64_FPU_H

#include <stdint.h>

/* Per-process x87/SSE register state, in the 512-byte FXSAVE/FXRSTOR
 * layout. The kernel itself still builds with -mno-sse -mno-80387 and so
 * never touches these registers -- they belong entirely to whichever
 * ring-3 process is running, which is what makes a plain eager save/
 * restore at every dispatch (see usermode.c's dispatch()) sufficient,
 * with no lazy CR0.TS/#NM trapping needed. FXSAVE/FXRSTOR require
 * 16-byte alignment: every area handed to fpu_save()/fpu_restore() must
 * live in the static process table (exec/process.c), never on a kernel
 * stack, whose alignment nothing here guarantees. */
#define FPU_STATE_SIZE 512

/* Enables x87/SSE for ring 3: clears CR0.EM/TS, sets CR0.MP/NE and
 * CR4.OSFXSR/OSXMMEXCPT. Before this, the first SSE instruction any
 * userland program executes raises #UD (or #NM). */
void fpu_init(void);

/* Fills `area` with the clean initial state a fresh process starts with
 * (what FNINIT plus the reset MXCSR would leave): every x87/SIMD
 * exception masked, round-to-nearest, empty x87 stack, zeroed XMM regs.
 * A plain zero-filled area is *not* that -- FCW=0/MXCSR=0 unmask every
 * floating-point exception. Plain C, so `area` may be anywhere. */
void fpu_state_init(uint8_t *area);

void fpu_save(uint8_t *area);
void fpu_restore(const uint8_t *area);

#endif
