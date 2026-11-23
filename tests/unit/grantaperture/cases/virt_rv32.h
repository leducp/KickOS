// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The virt_rv32 windows, from QEMU virt_memmap.

#ifndef KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_VIRT_RV32_H
#define KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_VIRT_RV32_H

#include "../aperture_case.h"

constexpr ApertureCase CASES[] = {
    {"TEST", 0x00100000u, 0x1000u, -KOS_EINVAL},
    {"CLINT", 0x02000000u, 0x10000u, -KOS_EINVAL},
    {"PLIC", 0x0C000000u, 0x1000u, -KOS_EINVAL},
    {"RTC", 0x00101000u, 0x1000u, 0},
};

#endif
