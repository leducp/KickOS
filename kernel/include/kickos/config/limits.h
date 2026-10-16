// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Structural / fixed configuration: design invariants, NOT user knobs. Each is coupled
// to the implementation and must not be overridden per app.

#ifndef KICKOS_CONFIG_LIMITS_H
#define KICKOS_CONFIG_LIMITS_H

#include <kickos/config/mpu_geometry.h> // KICKOS_MPU_MAX_REGIONS (generated)
#include <kickos/config/priorities.h> // the scheduler's priority range (generated)
#include <kickos/poll_bound.h>         // KICKOS_POLL_SPIN_MAX

#endif
