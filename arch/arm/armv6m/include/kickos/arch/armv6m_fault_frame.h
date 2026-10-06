// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Whether the v6-M fault reporter may read the stacked frame. v6-M latches no stacking abort,
// and a frame the hardware stacked at a wild PSP read from HardFault faults again, which locks
// the core up.

#ifndef KICKOS_ARCH_ARMV6M_FAULT_FRAME_H
#define KICKOS_ARCH_ARMV6M_FAULT_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h>

// r0-r3, r12, lr, pc, xPSR.
constexpr size_t ARMV6M_BASIC_FRAME_BYTES = 32u;

// On MSP, inside the running thread's own stack, or inside its kernel block, where syscall
// dispatch runs through the PSP.
static inline bool armv6m_fault_frame_readable(void const* frame, uint32_t exc_return)
{
    if ((exc_return & 0x4u) == 0u or kickos_fault_frame_trusted(frame, ARMV6M_BASIC_FRAME_BYTES))
    {
        return true;
    }
#if KICKOS_KERNEL_STACKS
    return kickos_fault_frame_on_kernel_stack(frame, ARMV6M_BASIC_FRAME_BYTES);
#else
    return false;
#endif
}

#endif
