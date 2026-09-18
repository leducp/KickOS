// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Default PMSAv7 commit, kept in a separate archive member for chip overrides.
// Mask interrupts while disabling, programming, and re-enabling the MPU.
// SYSMPU and PMSAv8 overrides read the same kickos_arm_mpu_pending state.
// Build even without MPU support because the switch path references this symbol.

#include <kickos/arch/arch.h>

#include "bench_mpu.h"

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU

extern "C"
{
    struct arch_mpu_encoded const* kickos_arm_mpu_pending(void);
    void kickos_arm_mpu_program(struct arch_mpu_encoded const* img);

    void kickos_arch_mpu_commit(void)
    {
#if KICKOS_BENCH
        uint32_t const bench_start = kickos_arm_mpu_bench_cyc();
#endif
        struct arch_mpu_encoded const* const img = kickos_arm_mpu_pending();
        // Do not record an empty commit: it writes no descriptors.
        if (img == nullptr)
        {
            return;
        }
        uint32_t primask;
        __asm volatile("mrs %0, primask" : "=r"(primask));
        __asm volatile("cpsid i" ::: "memory");
        kickos_arm_mpu_program(img);
        __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
#if KICKOS_BENCH
        kickos_bench_mpu_commit(kickos_arm_mpu_bench_cyc() - bench_start);
#endif
    }
}

#else

extern "C" void kickos_arch_mpu_commit(void)
{
}

#endif
