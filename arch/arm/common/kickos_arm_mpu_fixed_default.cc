// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// No chip fixed regions. The i.MX RT1062 (anti-speculation wrap) defines its own.
// Added to the arch library only under KICKOS_HAVE_MPU, the posture that declares the
// region type and calls this.

#include <kickos/arch/arch.h>

#include "mpu.h"

#include <stddef.h>

extern "C" size_t kickos_arm_mpu_fixed(struct kickos_arm_mpu_fixed_region const** out)
{
    (void)out;
    return 0;
}
