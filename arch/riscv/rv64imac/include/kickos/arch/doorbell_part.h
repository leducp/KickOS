// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the shared doorbell protocol takes from THIS PART: the padding a cell owes, the spin a
// wait costs, the console a refusal reaches, and the wording of each refusal.
//
// THE WORDING IS THE PART'S. tests/integration/check_smp_doorbell.sh greps each refusal as this
// backend's own literal, and one spelling shared across backends would leave its arms grepping
// an image for lines that image cannot print.

#ifndef KICKOS_ARCH_DOORBELL_PART_H
#define KICKOS_ARCH_DOORBELL_PART_H

#include <kickos/arch/arch.h>

#include <stddef.h>

// A CHOICE, not a measurement: RISC-V publishes no cache-line width in any CSR.
#define KICKOS_DOORBELL_LINE 64u

namespace kickos::doorbell
{
    // No hint instruction in this baseline: Zihintpause is not in the board's ISA string.
    inline void part_spin(void)
    {
        __asm volatile("nop" ::: "memory");
    }

    inline void part_write(char const* buf, size_t n)
    {
        arch_console_write(buf, n);
    }

    constexpr char PART_UNANSWERED[] = "KickOS: rv64 doorbell unanswered by core ";
    constexpr char PART_EARLY_WAIT[]
        = "KickOS: rv64 doorbell wait returned early, rounds settled 0x";
    constexpr char PART_NO_CONTEND[] = "KickOS: rv64 peers never completed an acquisition, held ";
    constexpr char PART_NO_SPIN[] = "KickOS: rv64 peers never reached the acquire loop, seen 0x";
}

#endif
