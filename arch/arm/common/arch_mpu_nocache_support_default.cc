// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// PMSA carries a memory type in every region descriptor (PMSAv7 RASR TEX/S/C/B, PMSAv8
// RLAR AttrIndx). Two chips answer for themselves in their own always-anchored TU:
// mk64f (a SYSMPU descriptor names no memory type) and imxrt1062 (the one ARM part in
// tree with a D-cache).

#include <kickos/arch/arch.h>

#include <stddef.h>

extern "C" int arch_mpu_nocache_support(void)
{
#if KICKOS_HAVE_MPU
    if (arch_mpu_min_region() != 0)
    {
        return ARCH_MPU_NOCACHE_PROGRAMMED;
    }
#endif
    return ARCH_MPU_NOCACHE_ALREADY;
}
