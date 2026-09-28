// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_DOORBELL_PART_H
#define KICKOS_ARCH_DOORBELL_PART_H

#include <kickos/arch/arch.h>

#include <stddef.h>

// ESP32-C6 HP SRAM supports atomics across its two cores. A line per writer
// avoids sharing an ownership unit if a later silicon revision adds caching.
#define KICKOS_DOORBELL_LINE 64u

namespace kickos::doorbell
{
    inline void part_spin(void) { __asm volatile("nop" ::: "memory"); }
    inline void part_write(char const* buf, size_t n) { arch_console_write(buf, n); }

    constexpr char PART_UNANSWERED[] = "KickOS: C6 doorbell unanswered by core ";
    constexpr char PART_EARLY_WAIT[] = "KickOS: C6 doorbell wait returned early, rounds settled 0x";
    constexpr char PART_NO_CONTEND[] = "KickOS: C6 peers never completed an acquisition, held ";
    constexpr char PART_NO_SPIN[] = "KickOS: C6 peers never reached the acquire loop, seen 0x";
    constexpr char PART_STILL_SETTLED[] =
        "KickOS: C6 doorbell round settled a request that never moved, peers ";
}

#endif
