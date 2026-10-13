// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// An rv32 CLINT's machine timer: MTIME and MTIMECMP are 64-bit registers an rv32 hart reaches as
// two 32-bit halves, the low half at the lower address. The chip supplies the addresses and the
// tick rate.

#ifndef KICKOS_ARCH_RISCV_COMMON_CLINT_RV32_H
#define KICKOS_ARCH_RISCV_COMMON_CLINT_RV32_H

#include <kickos/io/mmio.h>

#include <stdint.h>

namespace kickos::rv32::clint
{
    // The high half is read again to catch a low-half carry between the two reads.
    inline uint64_t read(uintptr_t mtime)
    {
        uint32_t hi = 0;
        uint32_t lo = 0;
        uint32_t hi2 = 0;
        do
        {
            hi = r32(mtime + 4u);
            lo = r32(mtime);
            hi2 = r32(mtime + 4u);
        } while (hi != hi2);
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    // The high half parks at its maximum first, so no match fires between the two stores.
    inline void arm(uintptr_t mtimecmp, uint64_t ticks)
    {
        r32(mtimecmp + 4u) = 0xFFFFFFFFu;
        r32(mtimecmp) = static_cast<uint32_t>(ticks);
        r32(mtimecmp + 4u) = static_cast<uint32_t>(ticks >> 32);
    }

    inline void disarm(uintptr_t mtimecmp)
    {
        r32(mtimecmp) = 0xFFFFFFFFu;
        r32(mtimecmp + 4u) = 0xFFFFFFFFu;
    }
}

#endif
