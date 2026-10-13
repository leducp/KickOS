// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The wait bound shared by the chip's wait for a released core to reach its entry and the
// GICv3 backend's waits.

#ifndef KICKOS_ARCH_ARM64_COMMON_SMP_BRINGUP_H
#define KICKOS_ARCH_ARM64_COMMON_SMP_BRINGUP_H

#include <stdint.h>

namespace kickos
{
    // Under emulation without icount the guest clock tracks HOST time, so a contended host
    // spends this budget while the guest barely executes: keep it a duration, never an
    // iteration count, and far over the need.
    constexpr uint64_t ARM64_BRINGUP_WAIT_NS = 5ull * 1000ull * 1000ull * 1000ull;
}

#endif
