// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The sam3x8e windows, from SAM3X Figure 7-1.

#ifndef KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_SAM3X8E_H
#define KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_SAM3X8E_H

#include "../aperture_case.h"

constexpr ApertureCase CASES[] = {
    {"SMC", 0x400E0000u, 0x200u, -KOS_EINVAL},
    {"MATRIX", 0x400E0400u, 0x200u, -KOS_EINVAL},
    {"PMC", 0x400E0600u, 0x200u, -KOS_EINVAL},
    {"EEFC0", 0x400E0A00u, 0x200u, -KOS_EINVAL},
    {"PIOA", 0x400E0E00u, 0x200u, -KOS_EINVAL},
    {"RSTC", 0x400E1A00u, 0x10u, -KOS_EINVAL},
    {"SUPC", 0x400E1A10u, 0x10u, -KOS_EINVAL},
    {"TC0", 0x40080000u, 0x4000u, -KOS_EPERM},
    {"SPI0", 0x40008000u, 0x4000u, 0},
    {"WDT", 0x400E1A50u, 0x10u, 0},
    KICKOS_CASE_ARMV7M_MPU,
};

#endif
