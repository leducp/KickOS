// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 FLASH0 controller registers. Clean-room from the XMC4700/XMC4800
// Reference Manual (V1.3, 2016-07, flash chapter). FCON.WSPFLASH must be widened
// BEFORE fCPU is raised, else an instruction fetch faults (RM 8.4.4).

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_REGS_FLASH_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_REGS_FLASH_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::xmc::reg::flash
{
    constexpr uintptr_t FCON = mmap::FLASH0_CTRL_BASE + 0x1014;

    // WSPFLASH[3:0]: flash read wait-states in fCPU cycles. Field 4 at 144 MHz.
    constexpr uint32_t FCON_WSPFLASH_MASK = 15u << 0;
    constexpr uint32_t FCON_WSPFLASH_4CYC = 4u << 0;
}

#endif
