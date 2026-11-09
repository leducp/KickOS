// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The CMSDK APB dual timer's first counter (ARM DDI 0479C sec.4.4.4), at the base the configured
// chip's file places it: shared by the mps2 chip and the an505 chip that compiles its sources.

#ifndef KICKOS_ARCH_ARM_CHIP_MPS2_REGS_DUALTIMER_H
#define KICKOS_ARCH_ARM_CHIP_MPS2_REGS_DUALTIMER_H

#include <chip_layout.h>

#include <stdint.h>

namespace kickos::mps2::reg::dualtimer
{
    constexpr uintptr_t TIMER1LOAD = KICKOS_LAYOUT_DUALTIMER_BASE + 0x00u;
    constexpr uintptr_t TIMER1VALUE = KICKOS_LAYOUT_DUALTIMER_BASE + 0x04u; // counts down
    constexpr uintptr_t TIMER1CONTROL = KICKOS_LAYOUT_DUALTIMER_BASE + 0x08u;
    constexpr uintptr_t TIMER1INTCLR = KICKOS_LAYOUT_DUALTIMER_BASE + 0x0Cu; // any write clears
    constexpr uintptr_t TIMER1RIS = KICKOS_LAYOUT_DUALTIMER_BASE + 0x10u;

    constexpr uint32_t CONTROL_ENABLE = 1u << 7;
    constexpr uint32_t CONTROL_INTENABLE = 1u << 5; // set at reset
    constexpr uint32_t CONTROL_SIZE32 = 1u << 1;    // 16-bit at reset
    // Mode bit 6 clear is free-running: past zero the count restarts at 0xFFFFFFFF, and the
    // interrupt latches at zero (sec.4.4.1, sec.4.4.2).

    constexpr uint32_t RIS_INT = 1u << 0;
}

#endif
