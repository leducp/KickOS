// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The mk64f windows, from K64 RM Tables 4-2 and 4-3.

#ifndef KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_MK64F_H
#define KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_MK64F_H

#include "../aperture_case.h"

constexpr ApertureCase CASES[] = {
    {"AIPS0", 0x40000000u, 0x1000u, -KOS_EINVAL},
    {"AXBS", 0x40004000u, 0x1000u, -KOS_EINVAL},
    {"SYSMPU", 0x4000D000u, 0x1000u, -KOS_EINVAL},
    {"FMC", 0x4001F000u, 0x1000u, -KOS_EINVAL},
    {"FTFE", 0x40020000u, 0x1000u, -KOS_EINVAL},
    {"SIM", 0x40048000u, 0x1000u, -KOS_EINVAL},
    {"PORTA", 0x40049000u, 0x1000u, -KOS_EINVAL},
    {"MCG", 0x40064000u, 0x1000u, -KOS_EINVAL},
    {"OSC", 0x40065000u, 0x1000u, -KOS_EINVAL},
    {"LLWU", 0x4007C000u, 0x1000u, -KOS_EINVAL},
    {"PMC", 0x4007D000u, 0x1000u, -KOS_EINVAL},
    {"SMC", 0x4007E000u, 0x1000u, -KOS_EINVAL},
    {"RCM", 0x4007F000u, 0x1000u, -KOS_EINVAL},
    {"AIPS1", 0x40080000u, 0x1000u, -KOS_EINVAL},
    {"PIT", 0x40037000u, 0x1000u, -KOS_EPERM},
    {"UART1", 0x4006B000u, 0x1000u, 0},
    {"EWM", 0x40061000u, 0x1000u, 0},
    {"WDOG", 0x40052000u, 0x1000u, 0},
    KICKOS_CASE_ARMV7M_MPU,
};

#endif
