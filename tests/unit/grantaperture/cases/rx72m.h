// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The rx72m windows, from RX72M UM Table 5.1.

#ifndef KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_RX72M_H
#define KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_RX72M_H

#include "../aperture_case.h"

constexpr ApertureCase CASES[] = {
    {"SYSTEM", 0x00080000u, 0x400u, -KOS_EINVAL},
    {"FLASH", 0x00081000u, 0x100u, -KOS_EINVAL},
    {"BSC", 0x00083000u, 0x100u, -KOS_EINVAL},
    {"MPU", 0x00086400u, 0x100u, -KOS_EINVAL},
    {"ICU", 0x00087000u, 0x100u, -KOS_EINVAL},
    {"WDT", 0x00088020u, 0x10u, 0},
    {"MPC", 0x0008C100u, 0x100u, -KOS_EINVAL},
    {"SYSTEM_LP", 0x0008C280u, 0x40u, -KOS_EINVAL},
    {"FACI", 0x007FE000u, 0x100u, -KOS_EINVAL},
    {"CMTW0", 0x00094200u, 0x100u, -KOS_EPERM},
    {"SCI0", 0x0008A000u, 0x20u, 0},
    {"IWDT", 0x00088030u, 0x10u, 0},
};

#endif
