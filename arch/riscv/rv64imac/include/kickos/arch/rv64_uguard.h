// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What sstatus.SUM, read back after kickos_rv64_init clears it, says about the guard.

#ifndef KICKOS_ARCH_RV64_UGUARD_H
#define KICKOS_ARCH_RV64_UGUARD_H

#include <kickos/arch/rv64_frame.h>
#include <kickos/diag.h>

#include <stdint.h>

namespace kickos
{
    namespace rv64
    {
        // Why a hart whose read-back sstatus is `sstatus` cannot keep S-mode out of user pages,
        // or null.
        inline char const* sum_refusal(uint64_t sstatus)
        {
            if ((sstatus & KICKOS_RV64_SSTATUS_SUM) != 0)
            {
                return diag::kSumNotCleared;
            }
            return nullptr;
        }

        // The banner's name for the guard a read-back of sstatus shows.
        inline char const* sum_posture(uint64_t sstatus)
        {
            if ((sstatus & KICKOS_RV64_SSTATUS_SUM) != 0)
            {
                return "sum set";
            }
            return "sum clear";
        }
    }
}

#endif
