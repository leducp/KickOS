// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The mps2 windows, from DAI0385D Tables 3-1 and 3-4.

#ifndef KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_MPS2_H
#define KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_MPS2_H

#include "../aperture_case.h"

constexpr ApertureCase CASES[] = {
    {"WATCHDOG", 0x40008000u, 0x1000u, 0},
    {"TEST_SLAVE", 0x4000B000u, 0x1000u, -KOS_EINVAL},
    {"SYSCTRL", 0x4001F000u, 0x1000u, -KOS_EINVAL},
    {"SCC", 0x4002F000u, 0x1000u, -KOS_EINVAL},
    {"DUALTIMER", 0x40002000u, 0x1000u, -KOS_EPERM},
    {"UART0", 0x40004000u, 0x1000u, 0},
    KICKOS_CASE_ARMV7M_MPU,
};

#endif
