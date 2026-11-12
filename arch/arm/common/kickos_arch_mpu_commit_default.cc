// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Default PMSAv7 commit, its own archive member so a chip override replaces it. Built without
// MPU support too: the switch path references the symbol. Interrupts stay masked while the MPU
// is disabled, programmed and re-enabled.

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU

extern "C"
{
    struct arch_mpu_encoded const* kickos_arm_mpu_pending(void);
    void kickos_arm_mpu_program(struct arch_mpu_encoded const* img);

    void kickos_arch_mpu_commit(void)
    {
        struct arch_mpu_encoded const* const img = kickos_arm_mpu_pending();
        // An empty commit writes no descriptors and is not recorded.
        if (img == nullptr)
        {
            return;
        }
        uint32_t primask;
        __asm volatile("mrs %0, primask" : "=r"(primask));
        __asm volatile("cpsid i" ::: "memory");
        kickos_arm_mpu_program(img);
        __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
    }
}

#else

extern "C" void kickos_arch_mpu_commit(void)
{
}

#endif
