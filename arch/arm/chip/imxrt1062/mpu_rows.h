// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The i.MX RT1062 fixed MPU rows (kickos_arm_mpu_fixed). Row 0 covers all 4 GiB with no access
// as Device and execute-never; the rows above it give privileged code back only the memory the
// image uses. ERR011573 / Arm 1013783-B: an address left to the default map is Normal, and an M7
// speculating into an unbacked AHB slave stalls with NO fault.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_MPU_ROWS_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_MPU_ROWS_H

#include <stddef.h>
#include <stdint.h>

#include "mpu.h"
#include "regs.h"

namespace kickos::imxrt1062::mpu_rows
{
    // RASR.SIZE for a region of 2^log2 bytes.
    constexpr uint32_t size_field(uint32_t log2)
    {
        return (log2 - 1u) << 1;
    }

    constexpr bool is_region(uint32_t base, uint32_t size)
    {
        return size >= 32u and (size & (size - 1u)) == 0u and (base & (size - 1u)) == 0u;
    }

    // TEX=001 C=1 B=1 S=0 (DDI0403E Table B3-13): what the default map gives SRAM.
    constexpr uint32_t MEM_NORMAL_WBWA = (1u << 19) | (1u << 17) | (1u << 16);
    constexpr uint32_t AP_PRW = 0x1u << 24;
    constexpr uint32_t DENY =
        arm::MPU_RASR_ENABLE | arm::MPU_RASR_AP_NONE | arm::MPU_RASR_XN | arm::MPU_RASR_MEM_DEVICE;

    // AIPS-1 to AIPS-4 at 0x4000_0000 (4 MiB) and AIPS-5 at 0x4200_0000 (1 MiB), IMXRT1060RM
    // Table 3-1: subregions 0 and 4 of a 64 MiB row. The reserved rest of each 8 MiB subregion
    // is Device too, which nothing speculates into.
    constexpr uint32_t PERIPH_BASE = 0x40000000u;
    constexpr uint32_t PERIPH_SRD = 0xEEu << 8;

    constexpr size_t COUNT = 4;

    struct Rows
    {
        kickos_arm_mpu_fixed_region row[COUNT];
    };

    // The background is row 0 so that every row above it wins over it (PMSAv7: highest region
    // wins). The other three do not overlap.
    constexpr Rows rows(uint32_t flash_base, uint32_t flash_size, uint32_t ram_base, uint32_t ram_size)
    {
        uint32_t const flash_log2 = static_cast<uint32_t>(__builtin_ctz(flash_size));
        uint32_t const ram_log2 = static_cast<uint32_t>(__builtin_ctz(ram_size));
        return Rows{{
            {0u, DENY | size_field(32u)},
            {ram_base,
             arm::MPU_RASR_ENABLE | size_field(ram_log2) | AP_PRW | arm::MPU_RASR_XN | MEM_NORMAL_WBWA},
            {PERIPH_BASE, arm::MPU_RASR_ENABLE | size_field(26u) | PERIPH_SRD | AP_PRW | arm::MPU_RASR_XN
                              | arm::MPU_RASR_MEM_DEVICE},
            {flash_base,
             arm::MPU_RASR_ENABLE | size_field(flash_log2) | arm::MPU_RASR_AP_PRO | arm::MPU_RASR_MEM_NORMAL},
        }};
    }
}

#endif
