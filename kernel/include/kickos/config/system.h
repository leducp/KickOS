// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// User / app provisioning knobs (static-allocation-first, sized per app). The
// integer pool sizes are CMake-`-D` overridable; the ns knob is a typed constexpr.

#ifndef KICKOS_CONFIG_SYSTEM_H
#define KICKOS_CONFIG_SYSTEM_H

#include <stdint.h>

#include <kickos/units.h>

// Max registered kernel objects for the M0 sim handle tables.
#ifndef KICKOS_MAX_SEMAPHORES
#define KICKOS_MAX_SEMAPHORES 16
#endif

// Static thread pool the syscall thread_spawn draws from (+ its kernel stacks).
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 16
#endif
#ifndef KICKOS_USER_STACK_SIZE
#define KICKOS_USER_STACK_SIZE (64 * 1024)
#endif

namespace kickos
{
    // Classic periodic-tick period -- only meaningful in the opt-in
    // KICKOS_SCHED_PERIODIC_TICK build (the tickless default arms per event).
    constexpr uint64_t KICKOS_TICK_PERIOD_NS = 1_ms;
}

#endif
