#include "fpu.h"
#include "../../lib/string.h"

#include <stdint.h>

#define CR0_MP (1ull << 1)
#define CR0_EM (1ull << 2)
#define CR0_TS (1ull << 3)
#define CR0_NE (1ull << 5)
#define CR4_OSFXSR (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)

#define FCW_DEFAULT 0x037F    /* all x87 exceptions masked, 64-bit precision, round-nearest */
#define MXCSR_DEFAULT 0x1F80u /* all SIMD exceptions masked, round-nearest */

void fpu_init(void) {
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    asm volatile("mov %0, %%cr4" : : "r"(cr4));

    asm volatile("fninit");
}

void fpu_state_init(uint8_t *area) {
    memset(area, 0, FPU_STATE_SIZE);
    /* FXSAVE layout: FCW at offset 0, abridged FTW at 4 (0 = every
     * register empty), MXCSR at 24. */
    area[0] = FCW_DEFAULT & 0xFF;
    area[1] = FCW_DEFAULT >> 8;
    area[24] = MXCSR_DEFAULT & 0xFF;
    area[25] = (MXCSR_DEFAULT >> 8) & 0xFF;
}

void fpu_save(uint8_t *area) {
    asm volatile("fxsave64 (%0)" : : "r"(area) : "memory");
}

void fpu_restore(const uint8_t *area) {
    asm volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
}
