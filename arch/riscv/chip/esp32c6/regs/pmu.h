// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 PMU registers the AMP doorbell drives (TRM v1.2, 12.9.1 PMU Register Summary).

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_PMU_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_PMU_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::pmu
{
    constexpr uintptr_t HP_INT_CLR = mmap::PMU_BASE + 0x168u;
    constexpr uintptr_t LP_INT_ST = mmap::PMU_BASE + 0x170u;
    constexpr uintptr_t LP_INT_CLR = mmap::PMU_BASE + 0x178u;
    constexpr uintptr_t HP_LP_CPU_COMM = mmap::PMU_BASE + 0x184u;

    // The LP kernel's words, LP_INT_ST to HP_LP_CPU_COMM: node 1's one PMU row covers no other.
    constexpr uintptr_t LP_KERNEL_FIRST = LP_INT_ST;
    constexpr uintptr_t LP_KERNEL_LAST = HP_LP_CPU_COMM + 3u;
}

#endif
