// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARM PMSA fixed-region seam. A chip may declare THREAD-INVARIANT MPU regions that
// are programmed ONCE at init into the LOW descriptor slots; the per-thread grants
// (kickos_arm_mpu_program) sit ABOVE them, so a grant correctly overrides the fixed
// background (PMSAv7: highest-numbered region wins). Introduced for the i.MX RT1062
// M7 anti-speculation wrap (docs/design-teensy-mpu-hang.md): the fixed rows carry raw
// PMSAv7 base+RASR so a chip can encode AP/type values (no-access, priv-RO, Device,
// Strongly-ordered) that the portable R/W/X/DEV attr vocabulary cannot.

#ifndef KICKOS_ARCH_ARM_COMMON_MPU_H
#define KICKOS_ARCH_ARM_COMMON_MPU_H

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    struct kickos_arm_mpu_fixed_region
    {
        uint32_t base; // region base (naturally aligned to size)
        uint32_t rasr; // fully-encoded PMSAv7 MPU_RASR (ENABLE|size|AP|TEX/C/B|XN)
    };

    // Weak chip hook: the chip's always-present regions. Default returns 0 (none), so
    // every existing board is unchanged. A chip fills the LOW slots via this.
    size_t kickos_arm_mpu_fixed(struct kickos_arm_mpu_fixed_region const** out);

    // Program the chip's fixed regions into slots [0, k) and enable the MPU. Call from
    // the chip arch_init BEFORE the cache enable and before the scheduler starts. Caches
    // the fixed count so kickos_arm_mpu_program offsets per-thread grants above it.
    void kickos_arm_mpu_fixed_init(void);
}

#endif // KICKOS_ARCH_ARM_COMMON_MPU_H
