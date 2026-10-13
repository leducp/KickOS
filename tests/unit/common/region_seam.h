// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A region chip's arch facts for a host gate, answered by region_seam.cc from g_region_seam,
// which each gate linking it defines with its own values and may change between cases.

#ifndef KICKOS_TESTS_UNIT_COMMON_REGION_SEAM_H
#define KICKOS_TESTS_UNIT_COMMON_REGION_SEAM_H

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

struct RegionSeam
{
    uintptr_t ram_base = 0;
    size_t ram_size = 0;
    size_t min_region = 32u;
    int pow2 = 0;
    int nocache = ARCH_MPU_NOCACHE_ALREADY;
    struct arch_reserved_span reserved = {};
    struct arch_reserved_span bus_masters = {};
    int bitband = 0;
};

extern RegionSeam g_region_seam;

#endif
