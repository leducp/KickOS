// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What an unprivileged thread meets after a bring-up that found CCR inverted (check_ccr_plant.sh
// plants it): an unaligned halfword store to its stack completes, a write to the STIR faults
// instead of pending a line, and a hardware divide by zero faults instead of returning 0.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

namespace
{
    constexpr uintptr_t STIR = 0xE000EF00;
    // Never enabled by any image this probe builds, so a write that lands only pends it.
    constexpr uint32_t STIR_LINE = 31;

    void say(char const* text)
    {
        kos::print(text);
    }
}

int main(int, char**)
{
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    char line[64];
    ksnprintf(line, sizeof(line), "[ccrprobe] npriv %u\n", static_cast<unsigned>(control & 1u));
    say(line);
#if defined(CCRPROBE_UNALIGNED)
    alignas(4) uint8_t volatile cell[8] = {};
    uint32_t const value = 0x3020u;
    uint32_t back;
    // Inline asm: the toolchain flags forbid the compiler from emitting this access.
    __asm volatile("strh %1, [%2, #1]\n\tldrh %0, [%2, #1]"
                   : "=&r"(back)
                   : "r"(value), "r"(cell)
                   : "memory");
    ksnprintf(line, sizeof(line), "[ccrprobe] unaligned ok 0x%x\n", static_cast<unsigned>(back));
    say(line);
#elif defined(CCRPROBE_STIR)
    say("[ccrprobe] stir write\n");
    *reinterpret_cast<uint32_t volatile*>(STIR) = STIR_LINE;
    say("[ccrprobe] stir landed\n");
#elif defined(CCRPROBE_DIV0)
    say("[ccrprobe] div0\n");
    uint32_t volatile zero = 0;
    uint32_t const divisor = zero;
    uint32_t quotient;
    __asm volatile("udiv %0, %1, %2" : "=r"(quotient) : "r"(7u), "r"(divisor));
    ksnprintf(line, sizeof(line), "[ccrprobe] div0 returned 0x%x\n",
              static_cast<unsigned>(quotient));
    say(line);
#else
#error "CCRPROBE_UNALIGNED, CCRPROBE_STIR or CCRPROBE_DIV0 names the access"
#endif
    return 0;
}
