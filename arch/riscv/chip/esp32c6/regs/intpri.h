// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 INTPRI block (base 0x600C_5000). Only the software-settable FROM_CPU source
// triggers are used; enable/type/priority/threshold go through the 0x2000_1000 window
// (regs/plic.h). The C6 has no S-mode, so this doorbell raises a real machine interrupt for
// every logical software-inject line.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_INTPRI_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_INTPRI_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::intpri
{
    // INTPRI_CPU_INTR_FROM_CPU_n_REG, n = 0..3 (TRM Register 10.70).
    constexpr uintptr_t FROM_CPU_0 = mmap::INTPRI_BASE + 0x90u; // bit0: assert the source
}

#endif
