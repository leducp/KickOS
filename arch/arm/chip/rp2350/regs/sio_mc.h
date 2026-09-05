// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 SIO multicore register map (RP2350 datasheet RP-008373-DS-2, 3.1.11 Table 17),
// beside the GPIO-only regs/sio.h it shares a base with. Every register below is
// CORE-LOCAL despite one address: a read or write reaches the accessing core's own bank.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_SIO_MC_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_SIO_MC_H

#include <stdint.h>

#include "sio.h"

namespace kickos::rp2350::reg::sio
{
    // 0 on core 0, 1 on core 1 (3.1.2). NOT the Cortex-M33 PPB CPUID.
    constexpr uintptr_t CPUID = BASE + 0x000u;

    // Inter-processor FIFOs, four 32-bit entries each way (3.1.5).
    constexpr uintptr_t FIFO_ST = BASE + 0x050u;
    constexpr uintptr_t FIFO_WR = BASE + 0x054u;
    constexpr uintptr_t FIFO_RD = BASE + 0x058u;

    // Eight doorbell flags each way, one interrupt per core (3.1.6). OUT_* address the
    // OPPOSITE core's IN_* bits; IN_CLR acknowledges by writing 1s, and the interrupt
    // deasserts only once every IN bit is clear.
    //
    // RP2350-E2: writes at and above +0x180 alias the SIO hardware spinlocks and release
    // them spuriously. Nothing in this tree claims a SIO spinlock.
    constexpr uintptr_t DOORBELL_OUT_SET = BASE + 0x180u;
    constexpr uintptr_t DOORBELL_OUT_CLR = BASE + 0x184u;
    constexpr uintptr_t DOORBELL_IN_SET = BASE + 0x188u;
    constexpr uintptr_t DOORBELL_IN_CLR = BASE + 0x18Cu;
}

#endif
