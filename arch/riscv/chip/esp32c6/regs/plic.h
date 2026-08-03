// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 CPU interrupt controller, addressed through the UNDOCUMENTED 0x2000_1000
// window (see mmap.h). TRM v1.2 documents the controller only as INTPRI at 0x600C_5000
// (section 1.6, register map section 10.4.2), and these offsets are NOT INTPRI's: INTPRI
// has EIP_STATUS @0x08, PRI_n @0x0C+4n, THRESH @0x8C, CLEAR @0xA8. Do not "correct" one
// map to the other: the layout below is the one the inject doorbell was validated with
// on silicon, and applying INTPRI's offsets to it (or these to INTPRI) writes the
// priority of CPU int n into the threshold register and masks the line.
//
// mie bit N gates CPU int N (TRM section 1.6.2 item 2); enable/type/per-int priority/
// threshold live here.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_PLIC_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_PLIC_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::esp32c6::reg::plic
{
    constexpr uintptr_t MXINT_ENABLE = mmap::PLIC_MX_BASE + 0x00u;   // bit n: enable CPU int n
    constexpr uintptr_t MXINT_TYPE = mmap::PLIC_MX_BASE + 0x04u;     // bit n: 0=level 1=edge
    constexpr uintptr_t MXINT_CLEAR = mmap::PLIC_MX_BASE + 0x08u;    // bit n: edge-clear
    constexpr uintptr_t MXINT_PRI_BASE = mmap::PLIC_MX_BASE + 0x10u; // PRI_n @ +0x4*n, [3:0]=1..15
    constexpr uintptr_t MXINT_THRESH = mmap::PLIC_MX_BASE + 0x90u;   // fire when prio >= thresh
    constexpr uintptr_t MXINT_CLAIM = mmap::PLIC_MX_BASE + 0x94u;    // pending/claim

    // Per-CPU-int priority register address (PRI_n).
    inline constexpr uintptr_t pri(uint32_t cpu_int)
    {
        return MXINT_PRI_BASE + 4u * cpu_int;
    }
}

#endif
