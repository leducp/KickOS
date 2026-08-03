// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 Interrupt Priority Register block (INTPRI) (TRM v1.2 section 1.6 + ch.10;
// base 0x600C_5000 per Table 5.3-2). TRM section 1.6.2 makes this THE CPU interrupt
// controller, but the tree drives enable/type/priority/threshold through the
// 0x2000_1000 window instead (regs/plic.h says why), and touches INTPRI only for its
// software-settable FROM_CPU source triggers. That one doorbell carries every logical
// software-inject line: the C6 has no S-mode, so the arch's SSIP inject channel is a
// no-op and this raises a real machine interrupt instead.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_INTPRI_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_INTPRI_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::esp32c6::reg::intpri
{
    // INTPRI_CPU_INTR_FROM_CPU_n_REG, n = 0..3 (TRM Register 10.70).
    constexpr uintptr_t FROM_CPU_0 = mmap::INTPRI_BASE + 0x90u; // bit0: assert the source
}

#endif
