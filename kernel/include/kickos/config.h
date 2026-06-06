// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_CONFIG_H
#define KICKOS_CONFIG_H

// Scheduler priority levels. Priority 0 is reserved for the idle thread;
// higher number = higher priority (find-first-set on the ready bitmap).
#define KICKOS_NUM_PRIO 32
#define KICKOS_PRIO_IDLE 0
#define KICKOS_PRIO_MIN 1
#define KICKOS_PRIO_MAX (KICKOS_NUM_PRIO - 1)

// Max per-task MPU region descriptors carried in the TCB.
#define KICKOS_MPU_MAX_REGIONS 8

// Tickless minimum-delta guard: never arm a one-shot timer closer than this
// to "now" (ns), so we never program a compare that is already in the past.
#define KICKOS_TIMER_MIN_DELTA_NS 20000ull // 20 us

// Max registered kernel objects for the M0 sim handle tables.
#define KICKOS_MAX_SEMAPHORES 16
#define KICKOS_MAX_IRQ 32

#endif
