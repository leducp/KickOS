// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The PMP entries for a region set, apart from the CSR writes so a host test can run the
// encoder the commit programs from.

#ifndef KICKOS_ARCH_RISCV_RV32IMAC_PMP_ENCODE_H
#define KICKOS_ARCH_RISCV_RV32IMAC_PMP_ENCODE_H

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU
namespace kickos
{
    namespace rv32
    {
        // NAPOT encoding: for a region of size 2^k (k>=3) aligned to its size,
        // pmpaddr = (base>>2) | ((size>>3)-1); the trailing 1s encode the size.
        inline uint32_t pmp_napot_addr(uintptr_t base, size_t size)
        {
            return (static_cast<uint32_t>(base) >> 2) | ((static_cast<uint32_t>(size) >> 3) - 1u);
        }

        // cfg byte: A=NAPOT (0b11<<3) | R | W? | X?. attr is the U-mode rights; M-mode bypasses
        // these unlocked entries.
        inline uint8_t pmp_cfg(uint32_t attr)
        {
            uint32_t c = 0x18u | 0x1u; // NAPOT | R
            if (attr & ARCH_MPU_W)
            {
                c |= 0x2u;
            }
            if (attr & ARCH_MPU_X)
            {
                c |= 0x4u;
            }
            return static_cast<uint8_t>(c);
        }

        // A region PMP cannot name is left cfg 0, which grants no access: the encoding fails
        // closed.
        inline uint32_t pmp_encode(struct arch_mpu_region const* regions, size_t n,
                                   struct arch_mpu_encoded* out)
        {
            if (n > ARCH_MPU_ENCODED_SLOTS)
            {
                n = ARCH_MPU_ENCODED_SLOTS;
            }
            uint32_t seated = 0;
            uint8_t cfg[ARCH_MPU_ENCODED_SLOTS];
            size_t i = 0;
            for (; i < n; i++)
            {
                out->addr[i] = 0;
                cfg[i] = 0;
                if (arch_mpu_region_encodable(regions[i].base, regions[i].size))
                {
                    out->addr[i] = pmp_napot_addr(regions[i].base, regions[i].size);
                    cfg[i] = pmp_cfg(regions[i].attr);
                    seated |= static_cast<uint32_t>(1) << i;
                }
            }
            for (; i < ARCH_MPU_ENCODED_SLOTS; i++)
            {
                out->addr[i] = 0;
                cfg[i] = 0;
            }
            out->cfg[0] = static_cast<uint32_t>(cfg[0]) | (static_cast<uint32_t>(cfg[1]) << 8)
                        | (static_cast<uint32_t>(cfg[2]) << 16) | (static_cast<uint32_t>(cfg[3]) << 24);
            out->cfg[1] = static_cast<uint32_t>(cfg[4]) | (static_cast<uint32_t>(cfg[5]) << 8)
                        | (static_cast<uint32_t>(cfg[6]) << 16) | (static_cast<uint32_t>(cfg[7]) << 24);
            return seated;
        }
    }
}

#endif

#endif
