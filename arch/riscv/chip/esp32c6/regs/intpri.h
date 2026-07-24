// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 local interrupt controller (INTPRI) registers (TRM v1.2 section 1.6 +
// ch.10). Vestigial as a CPU interrupt controller on this core (the PLIC owns that);
// KickOS uses only its software-settable FROM_CPU source triggers -- the one
// doorbell that carries every logical software-inject line (C6 has no S-mode, so the
// arch's SSIP inject channel is a no-op and this raises a real machine interrupt).

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_INTPRI_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_INTPRI_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::esp32c6::reg::intpri
{
    constexpr uintptr_t FROM_CPU_0 = mmap::INTPRI_BASE + 0x90u; // bit0: assert the source
}

#endif
