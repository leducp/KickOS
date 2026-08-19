// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// PMSAv7 region encoding (F411/XMC on v7-M, RP2040/microbit on v6-M). A chip with a
// different MPU (K64F SYSMPU) or a v8-M core (arch_arm_pmsav8.cc) defines its own
// arch_mpu_encode beside its own commit.

#include <kickos/arch/arch.h>

#include "regs.h"

#include <bit>
#include <stddef.h>
#include <stdint.h>

// KICKOS_ARM_MPU_PMSAV7 already implies KICKOS_HAVE_MPU (CMakeLists.txt resolves the
// selector to NONE whenever the build has no MPU), so no separate KICKOS_HAVE_MPU test.
#if KICKOS_ARM_MPU == KICKOS_ARM_MPU_PMSAV7

namespace
{
    // One region as an MPU_RASR value. attr is the UNPRIVILEGED access (supervisor comes
    // from the PRIVDEFENA background region): a code region is R+X (RO, executable); data
    // / stack / device is RW + execute-never. Device memory gets the ordered device type.
    // `size` is a power of two >= 32 with a naturally aligned base, which is what
    // arch_mpu_region_encodable admits.
    uint32_t mpu_rasr(size_t size, uint32_t attr)
    {
        using namespace kickos::arm;
        uint32_t const size_field = static_cast<uint32_t>(std::countr_zero(size)) - 1u;
        uint32_t rasr = MPU_RASR_ENABLE | (size_field << 1);
        if (attr & ARCH_MPU_X)
        {
            rasr |= MPU_RASR_AP_RO | MPU_RASR_MEM_NORMAL; // code: RO + executable
        }
        else if (attr & ARCH_MPU_DEV)
        {
            rasr |= MPU_RASR_AP_RW | MPU_RASR_XN | MPU_RASR_MEM_DEVICE; // MMIO
        }
        else if (attr & ARCH_MPU_NOCACHE)
        {
            rasr |= MPU_RASR_AP_RW | MPU_RASR_XN | MPU_RASR_MEM_NORMAL_NC; // bus-master shared
        }
        else
        {
            rasr |= MPU_RASR_AP_RW | MPU_RASR_XN | MPU_RASR_MEM_NORMAL; // data/stack
        }
        return rasr;
    }
}

extern "C" uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                                    struct arch_mpu_encoded* out)
{
    if (n > ARCH_MPU_ENCODED_SLOTS)
    {
        n = ARCH_MPU_ENCODED_SLOTS;
    }
    uint32_t seated = 0;
    size_t i = 0;
    for (; i < n; i++)
    {
        out->rbar[i] = 0;
        out->rasr[i] = 0;
        // RASR carries ctz(size) and RBAR masks the base down to a 32-byte boundary, so a
        // region PMSAv7 cannot name would be programmed as a window elsewhere. A
        // privileged thread's non-pow2 whole-arena grant lands here and runs on the
        // PRIVDEFENA background instead.
        if (arch_mpu_region_encodable(regions[i].base, regions[i].size))
        {
            out->rbar[i] = static_cast<uint32_t>(regions[i].base);
            out->rasr[i] = mpu_rasr(regions[i].size, regions[i].attr);
            seated |= static_cast<uint32_t>(1) << i;
        }
    }
    for (; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        out->rbar[i] = 0;
        out->rasr[i] = 0;
    }
    return seated;
}

#endif
