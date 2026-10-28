#pragma once

#include <stdint.h>

// The 3DS traps on the first floating-point instruction of a thread whose FP context has not been set up, and the
// crash dumps from the field all carry FPEXC = 0xC0000708 (EX|EN) with FPINST holding the trapping VFP instruction.
// Doing the setup ourselves at the top of every thread we create makes that path explicit instead of relying on
// whatever state the thread inherited.
static inline void N3DS_enableFpuForThisThread(void) {
    uint32_t cpacr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(cpacr));
    cpacr |= (0xFu << 20); // CP10 and CP11: full access
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 2" :: "r"(cpacr) : "memory");
    __asm__ volatile("isb");

    // Round to nearest, no traps, flush denormals to zero: the same settings the main thread applies at startup.
    uint32_t fpscr = 0x02400000u;
    __asm__ volatile("vmsr fpscr, %0" :: "r"(fpscr) : "memory");
}
