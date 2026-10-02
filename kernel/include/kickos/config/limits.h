// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Structural / fixed configuration: design invariants, NOT user knobs. Each is coupled
// to the implementation and must not be overridden per app.

#ifndef KICKOS_CONFIG_LIMITS_H
#define KICKOS_CONFIG_LIMITS_H

#include <kickos/config/priorities.h> // the scheduler's priority range (generated)

// Max per-task MPU region descriptors carried in the TCB (hardware-fixed: only
// 8 regions on ARMv6-M/v7-M).
#define KICKOS_MPU_MAX_REGIONS 8

// Structural bounded-spin backstop for raw synchronous MMIO polls on the
// panic/fault/boot path: a wedged peripheral must never hang, so every such poll
// caps here and bails. NOT a per-app knob: it dwarfs a real per-byte wait at any baud.
// Chip console-sync loops and the console-TX drain caps single-source this.
#define KICKOS_POLL_SPIN_MAX 1000000u

#endif
