// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The build's chip limits, as a chip that holds one task states them.

#ifndef KICKOS_TESTS_UNIT_LIBCLOCK_ONE_TASK_KICKOS_CHIP_LIMITS_H
#define KICKOS_TESTS_UNIT_LIBCLOCK_ONE_TASK_KICKOS_CHIP_LIMITS_H

#include_next <kickos/chip_limits.h>

#undef KICKOS_CHIP_ONE_TASK
#define KICKOS_CHIP_ONE_TASK 1

#endif
