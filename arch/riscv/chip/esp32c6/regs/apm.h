// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 access-permission (HP_APM, LP_APM, LP_APM0) and TEE registers. APM is the bus-side,
// per-security-mode unit IN SERIES with PMP: PMP checks first, then APM. The reset posture
// denies all REE access to every peripheral (region 0 catch-all), so a U-mode driver reaches
// nothing until node 0 programs the gate.
//
// APM QUIRKS:
//  - An APM denial does NOT trap (TRM 16.5): a read returns 0, a write is dropped,
//    and a separate HP_APM interrupt fires. Only PMP produces a load/store fault. So
//    per-thread isolation proofs must ride the PMP fault, never APM.
//  - Permissions are per security mode, never per master: every bus master but the two
//    CPUs is REE2 at power-up (TRM 16.3.2.3 Note), so a REE2 permit opens to DMA too.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_APM_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_APM_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::apm
{
    // --- HP_TEE mode controller. TEE_Mn_MODE_CTRL_REG = 0x00 + 0x4*n (Reg 16.53);
    //     HP CPU is master M0 (TRM 16.3.1). At MODE_TEE its user mode is REE0 and its machine
    //     mode TEE.
    constexpr uintptr_t HP_TEE_M0_MODE_CTRL = mmap::HP_TEE_BASE + 0x00u;

    // --- The LP CPU's mode (Reg 16.56, reset 3).
    constexpr uintptr_t LP_TEE_M0_MODE_CTRL = mmap::LP_TEE_BASE + 0x00u;
    // Set, the LP CPU reaches HP SRAM past every APM region (Reg 16.58, reset 0).
    constexpr uintptr_t LP_TEE_FORCE_ACC_HP = mmap::LP_TEE_BASE + 0x90u;
    constexpr uint32_t FORCE_ACC_MASK = 0x1u;

    // TEE_M0_MODE and LP_TEE_M0_MODE values, bits 1:0 (Reg 16.53, 16.56).
    constexpr uint32_t MODE_MASK = 0x3u;
    constexpr uint32_t MODE_TEE = 0u;
    constexpr uint32_t MODE_REE0 = 1u;
    constexpr uint32_t MODE_REE1 = 2u;
    constexpr uint32_t MODE_REE2 = 3u;

    // --- Region controllers. HP_APM (Reg 16.1-16.5), LP_APM (Reg 16.25-16.29) and LP_APM0
    //     (Reg 16.41-16.45) share one layout.
    constexpr uintptr_t HP_APM_BASE = mmap::HP_APM_BASE;
    constexpr uintptr_t LP_APM_BASE = mmap::LP_APM_BASE;
    constexpr uintptr_t LP_APM0_BASE = mmap::LP_APM0_BASE;
    constexpr uintptr_t REGION_STRIDE = 0x0Cu;

    // An address's low two bits are 0 (16.3.2.2); ATTR is bits 10:0 less 3 and 7 (Reg 16.4,
    // 16.28, 16.44).
    constexpr uint32_t ADDR_MASK = ~3u;
    constexpr uint32_t ATTR_MASK = 0x777u;

    // LP_APM0_DATE (Reg 16.52): bits 27:0, reset 0x2205240.
    constexpr uintptr_t LP_APM0_DATE = LP_APM0_BASE + 0x7FCu;
    constexpr uint32_t DATE_MASK = 0x0FFFFFFFu;
    constexpr uint32_t DATE = 0x2205240u;

    inline constexpr uintptr_t filter_en(uintptr_t gate)
    {
        return gate + 0x00u;
    }
    inline constexpr uintptr_t region_start(uintptr_t gate, uint32_t n)
    {
        return gate + 0x04u + REGION_STRIDE * n;
    }
    inline constexpr uintptr_t region_end(uintptr_t gate, uint32_t n) // inclusive
    {
        return gate + 0x08u + REGION_STRIDE * n;
    }
    inline constexpr uintptr_t region_attr_of(uintptr_t gate, uint32_t n)
    {
        return gate + 0x0Cu + REGION_STRIDE * n;
    }
    inline constexpr uintptr_t func_ctrl(uintptr_t gate) // each access path's enable
    {
        return gate + 0xC4u;
    }

    constexpr uintptr_t FILTER_EN = filter_en(HP_APM_BASE);

    inline constexpr uintptr_t region_addr_start(uint32_t n)
    {
        return region_start(HP_APM_BASE, n);
    }
    inline constexpr uintptr_t region_addr_end(uint32_t n)
    {
        return region_end(HP_APM_BASE, n);
    }
    inline constexpr uintptr_t region_attr(uint32_t n)
    {
        return region_attr_of(HP_APM_BASE, n);
    }

    inline constexpr uint32_t region_en(uint32_t n)
    {
        return 1u << n;
    }

    // ATTR: each REE mode owns X, W, R at bits 0, 1, 2 of its own nibble, REE0 the nibble at
    // bit 0, REE1 at bit 4, REE2 at bit 8.
    constexpr uint32_t ATTR_X = 1u << 0;
    constexpr uint32_t ATTR_W = 1u << 1;
    constexpr uint32_t ATTR_R = 1u << 2;
    constexpr uint32_t ATTR_MODE_MASK = ATTR_X | ATTR_W | ATTR_R;

    inline constexpr uint32_t attr_shift(uint32_t mode)
    {
        return 4u * (mode - MODE_REE0);
    }
}

#endif
