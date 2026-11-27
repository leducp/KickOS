// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The imx8mp windows the A53 cluster sees, from IMX8MPRM Tables 2-3 to 2-7 and 2.2.

#ifndef KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_IMX8MP_A53_H
#define KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_IMX8MP_A53_H

#include "../aperture_case.h"

constexpr ApertureCase CASES[] = {
    {"WDOG1", 0x30280000u, 0x10000u, 0},
    {"WDOG2", 0x30290000u, 0x10000u, 0},
    {"CCM", 0x30380000u, 0x10000u, -KOS_EINVAL},
    {"SRC", 0x30390000u, 0x10000u, -KOS_EINVAL},
    {"GICD", 0x38800000u, 0x10000u, -KOS_EINVAL},
};

#endif
