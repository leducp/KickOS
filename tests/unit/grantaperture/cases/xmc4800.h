// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The xmc4800 windows, from XMC4800 RM Table 7-2.

#ifndef KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_XMC4800_H
#define KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_XMC4800_H

#include "../aperture_case.h"

constexpr ApertureCase CASES[] = {
    {"PBA0", 0x40000000u, 0x4000u, -KOS_EINVAL},
    {"ERU1", 0x40044000u, 0x4000u, -KOS_EINVAL},
    {"PBA1", 0x48000000u, 0x4000u, -KOS_EINVAL},
    {"SCU", 0x50004000u, 0x4000u, -KOS_EINVAL},
    {"WDT", 0x50008000u, 0x4000u, 0},
    {"PMU0", 0x58000000u, 0x4000u, -KOS_EINVAL},
    {"PREFETCH", 0x58004000u, 0x4000u, -KOS_EINVAL},
    {"EBU", 0x58008000u, 0x4000u, -KOS_EINVAL},
    {"CCU40", 0x4000C000u, 0x4000u, -KOS_EPERM},
    {"VADC", 0x40004000u, 0x4000u, 0},
    KICKOS_CASE_ARMV7M_MPU,
};

#endif
