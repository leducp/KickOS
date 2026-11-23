// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One device window a spawn names, and the errno grant_device_window_admit answers it.

#ifndef KICKOS_TESTS_UNIT_GRANTAPERTURE_APERTURE_CASE_H
#define KICKOS_TESTS_UNIT_GRANTAPERTURE_APERTURE_CASE_H

#include <kickos/sys/errno.h>

#include <stddef.h>
#include <stdint.h>

struct ApertureCase
{
    char const* name;
    uintptr_t base;
    size_t size;
    int answer;
};

// The Cortex-M MPU's registers in the system control space.
#define KICKOS_CASE_ARMV7M_MPU {"MPU", 0xE000ED00u, 0x100u, -KOS_EINVAL}

#endif
