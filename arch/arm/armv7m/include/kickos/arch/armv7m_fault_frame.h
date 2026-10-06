// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the v7-M fault paths may believe: the stacked frame, and an address a bus-side unit
// latched outside the core's fault registers.

#ifndef KICKOS_ARCH_ARMV7M_FAULT_FRAME_H
#define KICKOS_ARCH_ARMV7M_FAULT_FRAME_H

#include <stddef.h>
#include <stdint.h>

// r0-r3, r12, lr, pc, xPSR.
constexpr size_t ARMV7M_BASIC_FRAME_BYTES = 32u;

// MSTKERR/MUNSTKERR (CFSR bits 4/3) and STKERR/UNSTKERR (bits 12/11): the hardware aborted the
// stacking or unstacking of the frame, so it never was or no longer is a frame. LSPERR/MLSPERR
// are not among them: lazy FP preservation leaves the integer frame intact.
constexpr uint32_t ARMV7M_CFSR_STACKING_ABORTS = 0x1818u;

// A frame the hardware finished stacking is readable wherever it sits.
static inline bool armv7m_fault_frame_readable(uint32_t cfsr)
{
    return (cfsr & ARMV7M_CFSR_STACKING_ABORTS) == 0u;
}

// CFSR fields (ARMv7-M ARM B3.2.15).
constexpr uint32_t ARMV7M_CFSR_MMFSR = 0xFFu;
constexpr uint32_t ARMV7M_CFSR_PRECISERR = 1u << 9;
constexpr uint32_t ARMV7M_CFSR_IMPRECISERR = 1u << 10;
constexpr uint32_t ARMV7M_CFSR_BFARVALID = 1u << 15;
constexpr uint32_t ARMV7M_CFSR_UFSR = 0xFFFF0000u;

// Whether an address arch_fault_chip_addr latched can explain this fault: a bus fault the core
// holds no address for, imprecise or precise with BFAR invalid, and no other fault beside it.
// An instruction fetch (IBUSERR, as IACCVIOL) keeps none: its stacked PC names the address.
static inline bool armv7m_chip_addr_explains(uint32_t cfsr)
{
    if ((cfsr & (ARMV7M_CFSR_UFSR | ARMV7M_CFSR_MMFSR)) != 0u)
    {
        return false;
    }
    if ((cfsr & ARMV7M_CFSR_IMPRECISERR) != 0u)
    {
        return true;
    }
    return (cfsr & ARMV7M_CFSR_PRECISERR) != 0u and (cfsr & ARMV7M_CFSR_BFARVALID) == 0u;
}

#endif
