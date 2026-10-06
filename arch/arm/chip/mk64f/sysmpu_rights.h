// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The user rights a SYSMPU region descriptor grants (K64 RM 19.3.6, MPU_RGDn_WORD2). The WORD2
// fields are per MPU logical bus master, and RM 3.3.7.2 Table 3-20 numbers the core 0 (code
// and system bus alike) and the debugger 1; the crossbar's port numbers (RM 3.3.6.1) are not
// these.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_SYSMPU_RIGHTS_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_SYSMPU_RIGHTS_H

#include <stdint.h>

#include <kickos/arch/arch.h>

// The core: M0UM[2:0], M0SM[4:3] (0b11 = as user, 0b00 = r/w/x).
constexpr uint32_t SYSMPU_WORD2_M0UM_X = 1u << 0;
constexpr uint32_t SYSMPU_WORD2_M0UM_W = 1u << 1;
constexpr uint32_t SYSMPU_WORD2_M0UM_R = 1u << 2;
constexpr uint32_t SYSMPU_WORD2_M0SM = 0x3u << 3;

// A thread's region grants the core's user mode alone. Supervisor reach is RGD0's, and the
// debugger's is RGD0's too, which the core cannot write (RM 3.3.7.5, Table 3-23).
static inline uint32_t sysmpu_word2(uint32_t attr)
{
    uint32_t w = SYSMPU_WORD2_M0UM_R;
    if ((attr & ARCH_MPU_W) != 0u)
    {
        w |= SYSMPU_WORD2_M0UM_W;
    }
    if ((attr & ARCH_MPU_X) != 0u)
    {
        w |= SYSMPU_WORD2_M0UM_X;
    }
    return w;
}

#endif
