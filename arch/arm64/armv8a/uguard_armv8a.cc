// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The choice of the form that keeps an AArch64 core's EL1 out of user pages.

#include "uguard_armv8a.h"

#include <kickos/diag.h>

namespace kickos
{
    namespace armv8a
    {
        UGuard uguard_of(uint64_t mmfr1)
        {
            if (((mmfr1 >> 20) & 0xFu) != 0)
            {
                return UGuard::PAN;
            }
            return UGuard::ROOT;
        }

        // One form for every core: a thread may resume on a core other than the one that built
        // its SPSR.
        char const* uguard_refusal(UGuard system, UGuard core)
        {
            if (system == UGuard::UNSET or system == core)
            {
                return nullptr;
            }
            return diag::kUguardMixed;
        }

        char const* uguard_root_refusal(uint64_t empty_ttbr)
        {
            if ((empty_ttbr >> 48) == 0)
            {
                return diag::kUguardNoAsid;
            }
            return nullptr;
        }
    }
}
