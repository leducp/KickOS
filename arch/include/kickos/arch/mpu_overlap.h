// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// How an MPU decides an access to a byte that two of its enabled regions cover. Each
// <kickos/arch/mpu_encoded.h> names its backend's rule as ARCH_MPU_OVERLAP.

#ifndef KICKOS_ARCH_MPU_OVERLAP_H
#define KICKOS_ARCH_MPU_OVERLAP_H

#define ARCH_MPU_OVERLAP_FAULTS 0 // the access faults, a privileged one included
#define ARCH_MPU_OVERLAP_HIGHER 1 // the higher-numbered region decides
#define ARCH_MPU_OVERLAP_LOWER 2  // the lower-numbered region decides
#define ARCH_MPU_OVERLAP_UNION 3  // any region granting the access admits it

#endif
