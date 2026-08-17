// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 FLASH interface register map (RM0383 sec.3.8.1): access-control /
// latency + prefetch + ART caches.

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_FLASH_H
#define KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_FLASH_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::stm32f411::reg::flash
{
    constexpr uintptr_t ACR = mmap::FLASH_BASE + 0x00u; // access control (RM lines 2784-2828)

    // At 3.3 V, HCLK=84 MHz -> 2 wait states (RM Table 5, line 2066).
    constexpr uint32_t ACR_LATENCY_2WS = 0x2u << 0;
    constexpr uint32_t ACR_PRFTEN = 1u << 8;  // prefetch enable
    constexpr uint32_t ACR_ICEN = 1u << 9;    // instruction cache (ART)
    constexpr uint32_t ACR_DCEN = 1u << 10;   // data cache
}

#endif
