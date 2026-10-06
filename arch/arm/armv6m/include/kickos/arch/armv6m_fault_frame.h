// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Whether the v6-M fault reporter may read the stacked frame. v6-M latches no stacking abort,
// and a frame the hardware stacked at a wild PSP read from HardFault faults again, which locks
// the core up.

#ifndef KICKOS_ARCH_ARMV6M_FAULT_FRAME_H
#define KICKOS_ARCH_ARMV6M_FAULT_FRAME_H

#include <stdint.h>

#include <kickos/arch/arch.h>

// On MSP, or inside the running thread's own stack.
static inline bool armv6m_fault_frame_readable(void const* frame, uint32_t exc_return)
{
    return (exc_return & 0x4u) == 0u or kickos_fault_frame_trusted(frame, 32);
}

#endif
