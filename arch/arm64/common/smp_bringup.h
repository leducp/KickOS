// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The wait bound the chip's release of a secondary is written against, shared between the
// chip's wait for a released core to reach its entry and the GICv3 backend's waits. The
// doorbell's own bring-up check owns its bound in arch/common/doorbell_protocol.cc, that one
// being the same figure for all three backends rather than arm64's.

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
