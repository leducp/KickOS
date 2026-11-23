// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Which SError taken from EL0 may end its thread's task rather than the system.

#ifndef KICKOS_ARCH_ARM64_ARMV8A_SERROR_ARMV8A_H
#define KICKOS_ARCH_ARM64_ARMV8A_SERROR_ARMV8A_H

#include <stdint.h>

namespace kickos
{
    namespace armv8a
    {
        // Whether ESR_EL1 `esr` names an SError that may end only its thread's task. Under FEAT_RAS
        // (ID_AA64PFR0_EL1 `pfr0`) that is one whose syndrome names a state other than
        // uncontainable (DDI 0487 M.b, ESR_EL1.AET); without it, every SError.
        bool serror_containable(uint64_t esr, uint64_t pfr0);
    }
}

#endif
