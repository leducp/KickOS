// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Structural / fixed configuration -- design invariants, NOT user knobs. These
// are coupled to the implementation and are not meant to be overridden per app.

#ifndef KICKOS_CONFIG_LIMITS_H
#define KICKOS_CONFIG_LIMITS_H

// Scheduler priority levels. Priority 0 is reserved for the idle thread; higher
// number = higher priority (find-first-set on the ready bitmap). NUM_PRIO is
// coupled to the uint32_t ready bitmap: raising it past 32 needs a hierarchical
// bitmap, not a bigger number.
#define KICKOS_NUM_PRIO 32
#define KICKOS_PRIO_IDLE 0
#define KICKOS_PRIO_MIN 1
#define KICKOS_PRIO_MAX (KICKOS_NUM_PRIO - 1)

// Max per-task MPU region descriptors carried in the TCB (hardware-fixed: only
// 8 regions on ARMv6-M/v7-M).
#define KICKOS_MPU_MAX_REGIONS 8

#endif
