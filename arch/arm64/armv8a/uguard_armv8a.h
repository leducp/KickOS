// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// How an AArch64 core keeps EL1 out of the pages EL0 can reach.

#ifndef KICKOS_ARCH_ARM64_ARMV8A_UGUARD_ARMV8A_H
#define KICKOS_ARCH_ARM64_ARMV8A_UGUARD_ARMV8A_H

#include <stdint.h>

namespace kickos
{
    namespace armv8a
    {
        enum class UGuard : uint8_t
        {
            UNSET = 0,
            PAN,  // PSTATE.PAN (ARMv8.1)
            ROOT, // a reserved TTBR0 root while at EL1
        };

        // PAN where ID_AA64MMFR1_EL1.PAN names it, the reserved root otherwise.
        UGuard uguard_of(uint64_t mmfr1);

        // Why a core whose guard is `core` cannot join a system the boot core put on `system`,
        // or null.
        char const* uguard_refusal(UGuard system, UGuard core);

        // Why the reserved-root form cannot run with this empty root's TTBR0 value, or null: under
        // ASID 0 the entries EL0 left in the TLB would answer at EL1.
        char const* uguard_root_refusal(uint64_t empty_ttbr);
    }
}

#endif
