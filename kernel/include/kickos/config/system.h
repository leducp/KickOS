// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// User / app provisioning knobs, each CMake-`-D` overridable. The pools' sizes are the
// system's, from its composition's budgets (<kickos/pools.h>).

#ifndef KICKOS_CONFIG_SYSTEM_H
#define KICKOS_CONFIG_SYSTEM_H

#include <stdint.h>

#include <kickos/units.h>
#include <kickos/config/stack_geometry.h> // KICKOS_STACK_ALIGN (generated)

// Use board_config.h provisioning when available; standalone defaults target
// the simulator rather than small-RAM boards.
#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif

// Maximum delegated capabilities per spawn. Staging costs 16 bytes per entry
// on the caller's stack, so this limit is independent of table width.
#ifndef KICKOS_MAX_SPAWN_GRANTS
#define KICKOS_MAX_SPAWN_GRANTS 6
#endif
// Windows one thread may hold, staged on the spawner's stack like the grant list.
#ifndef KICKOS_MAX_THREAD_WINDOWS
#define KICKOS_MAX_THREAD_WINDOWS 4
#endif
// Bytes of argv strings a task create copies, NULs included.
#ifndef KICKOS_ARGV_MAX
#define KICKOS_ARGV_MAX 256
#endif
// Strings in one task create's argv.
#ifndef KICKOS_MAX_ARGC
#define KICKOS_MAX_ARGC 32
#endif
// Virtual-range records per address space, independent of MPU descriptors.
// Image and reservation records persist; capability mappings release their
// records on unmap, and thread stacks release theirs at exit.
#ifndef KICKOS_ASPACE_RANGES
#define KICKOS_ASPACE_RANGES 96
#endif
// Maximum kernel instances in one address space. Storage grows per instance;
// a count of one compiles out instance lookup.
#ifndef KICKOS_MAX_INSTANCES
#define KICKOS_MAX_INSTANCES 1
#endif
// Stack a spawned thread gets when kos_thread_params carries no caller-owned
// stack_base/stack_size. A caller-supplied stack is validated against the floor and
// alignment below.
#ifndef KICKOS_USER_STACK_SIZE
#define KICKOS_USER_STACK_SIZE (64 * 1024)
#endif
// Minimum caller-supplied stack, sized for the deepest thread-exit path.
// CMake supplies an architecture-specific value; this fallback is conservative.
// Reject undersized or misaligned stacks. Idle is exempt because it never exits.
#ifndef KICKOS_MIN_STACK_SIZE
#define KICKOS_MIN_STACK_SIZE 1024
#endif

// The bootstrap idle/root thread stacks. A syscall runs on the calling thread's stack, so
// root must fit the deepest dispatch. Defaults suit the sim; a small-SRAM board overrides.
#ifndef KICKOS_IDLE_STACK_SIZE
#define KICKOS_IDLE_STACK_SIZE (64 * 1024)
#endif
#ifndef KICKOS_ROOT_STACK_SIZE
#define KICKOS_ROOT_STACK_SIZE (64 * 1024)
#endif

namespace kickos
{
    // Ignored unless KICKOS_SCHED_PERIODIC_TICK is opted into; the tickless
    // default arms per event and never reads it.
    constexpr uint64_t KICKOS_TICK_PERIOD_NS = 1_ms;
}

#endif
