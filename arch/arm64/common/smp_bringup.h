// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The one wait bound arm64 multicore bring-up is written against, shared so the figure has a
// single home: the chip's wait for a released core to reach its entry and the armv8a
// doorbell check's wait for a peer to reach the lock's acquire loop are the same quantity.

#ifndef KICKOS_ARCH_ARM64_COMMON_SMP_BRINGUP_H
#define KICKOS_ARCH_ARM64_COMMON_SMP_BRINGUP_H

#include <stdint.h>

namespace kickos
{
    // Sized far over rather than tuned: under emulation without icount the guest clock tracks
    // HOST time, so a contended host spends this budget while the guest barely executes. A
    // duration and never an iteration count for that same reason.
    constexpr uint64_t ARM64_BRINGUP_WAIT_NS = 5ull * 1000ull * 1000ull * 1000ull;
}

#endif
