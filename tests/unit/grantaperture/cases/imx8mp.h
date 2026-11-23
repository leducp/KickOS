// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The imx8mp windows, from IMX8MPRM Tables 2-3 to 2-7, the M7's map.

#ifndef KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_IMX8MP_H
#define KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_IMX8MP_H

#include "../aperture_case.h"

constexpr ApertureCase CASES[] = {
    {"AIPS1_CONFIG", 0x301F0000u, 0x10000u, -KOS_EINVAL},
    {"WDOG2", 0x30290000u, 0x10000u, 0},
    {"IOMUXC", 0x30330000u, 0x10000u, -KOS_EINVAL},
    {"OCOTP", 0x30350000u, 0x10000u, -KOS_EINVAL},
    {"ANA_PLL", 0x30360000u, 0x10000u, -KOS_EINVAL},
    {"CCM", 0x30380000u, 0x10000u, -KOS_EINVAL},
    {"SRC", 0x30390000u, 0x10000u, -KOS_EINVAL},
    {"GPC", 0x303A0000u, 0x10000u, -KOS_EINVAL},
    {"RDC", 0x303D0000u, 0x10000u, -KOS_EINVAL},
    {"CSU", 0x303E0000u, 0x10000u, -KOS_EINVAL},
    {"QSPI", 0x30BB0000u, 0x10000u, -KOS_EINVAL},
    {"TZASC", 0x32F80000u, 0x10000u, -KOS_EINVAL},
    {"MU_1_B", 0x30AB0000u, 0x10000u, -KOS_EPERM},
    {"I2C1", 0x30A20000u, 0x10000u, 0},
    {"WDOG3", 0x302A0000u, 0x10000u, 0},
    KICKOS_CASE_ARMV7M_MPU,
};

#endif
