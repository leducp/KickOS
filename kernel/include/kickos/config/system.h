// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// User / app provisioning knobs. Every pool size below is CMake-`-D` overridable.

#ifndef KICKOS_CONFIG_SYSTEM_H
#define KICKOS_CONFIG_SYSTEM_H

#include <stdint.h>

#include <kickos/units.h>

// Use board_config.h provisioning when available; standalone defaults target
// the simulator rather than small-RAM boards.
#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif

// CAP_SEM object pool.
#ifndef KICKOS_MAX_SEMAPHORES
#define KICKOS_MAX_SEMAPHORES 16
#endif

// Priority-inheritance mutex pool (CAP_MUTEX objects; see sync.h).
#ifndef KICKOS_MAX_MUTEXES
#define KICKOS_MAX_MUTEXES 8
#endif

// Endpoint (IPC rendezvous) pool (CAP_ENDPOINT objects; see endpoint.h).
// The u8 endpoint_refs counter bounds concurrent holders at runtime (obj_ref_inc
// refuses at the ceiling); it is not a compile-time bound on this knob.
#ifndef KICKOS_MAX_ENDPOINTS
#define KICKOS_MAX_ENDPOINTS 4
#endif

// Maximum spawned threads and their kernel stacks, excluding root.
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 16
#endif
// ThreadPool includes root's permanent slot. Use this count for TCB arrays
// and capability runs; use MAX_THREADS for spawned-stack provisioning.
#define KICKOS_THREAD_SLOTS (KICKOS_MAX_THREADS + 1)
// RAM limit for root's derived capability-table width. Child tables use
// separately sized slab runs; this limit does not multiply every table.
#ifndef KICKOS_CAP_TABLE_SUPPLY
#define KICKOS_CAP_TABLE_SUPPLY 16
#endif
// Maximum delegated capabilities per spawn. Staging costs 16 bytes per entry
// on the caller's stack, so this limit is independent of table width.
#ifndef KICKOS_MAX_SPAWN_GRANTS
#define KICKOS_MAX_SPAWN_GRANTS 6
#endif
// Virtual-range records per address space, independent of MPU descriptors.
// Image and reservation records persist; capability mappings release their
// records on unmap, and thread stacks release theirs at exit.
#ifndef KICKOS_ASPACE_RANGES
#define KICKOS_ASPACE_RANGES 40
#endif
// Lifetime arena ownership records for region backends. Exhaustion makes
// kos_ram_alloc return null; records are not individually freed.
#ifndef KICKOS_RAM_OWNER_SLOTS
#define KICKOS_RAM_OWNER_SLOTS 48
#endif
// Task groups, independent of thread count. Idle and root each use one slot.
// Plain spawns join an existing task; explicit tasks and separate grants need a slot.
#ifndef KICKOS_MAX_TASKS
#define KICKOS_MAX_TASKS 18
#endif
// Domain slots, including the two permanent kernel/default-user domains.
// Region backends can share the default domain; MMU tasks need separate domains.
#ifndef KICKOS_MAX_DOMAINS
#if KICKOS_HAVE_ASPACE
#define KICKOS_MAX_DOMAINS 20
#else
#define KICKOS_MAX_DOMAINS 18
#endif
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
#ifndef KICKOS_STACK_ALIGN
#define KICKOS_STACK_ALIGN 16
#endif

// The bootstrap idle/root thread stacks. A syscall runs on the calling thread's stack, so
// root must fit the deepest dispatch. Defaults suit the sim; a small-SRAM board overrides.
#ifndef KICKOS_IDLE_STACK_SIZE
#define KICKOS_IDLE_STACK_SIZE (64 * 1024)
#endif
#ifndef KICKOS_ROOT_STACK_SIZE
#define KICKOS_ROOT_STACK_SIZE (64 * 1024)
#endif

// Concurrently-registered tier-1 IRQ-as-event handles.
#ifndef KICKOS_MAX_IRQ_HANDLES
#define KICKOS_MAX_IRQ_HANDLES 8
#endif

// Frame RUNS a capability may name at once. Built only where a frame pool exists.
#ifndef KICKOS_MAX_FRAME_RUNS
#define KICKOS_MAX_FRAME_RUNS 8
#endif

// Per-task limits for charged pools. Each must leave at least one slot
// available in its corresponding global pool.
#ifndef KICKOS_TASK_SEMAPHORE_BUDGET
#define KICKOS_TASK_SEMAPHORE_BUDGET 15
#endif
#ifndef KICKOS_TASK_MUTEX_BUDGET
#define KICKOS_TASK_MUTEX_BUDGET 7
#endif
#ifndef KICKOS_TASK_ENDPOINT_BUDGET
#define KICKOS_TASK_ENDPOINT_BUDGET 3
#endif
#ifndef KICKOS_TASK_IRQ_HANDLE_BUDGET
#define KICKOS_TASK_IRQ_HANDLE_BUDGET 7
#endif

namespace kickos
{
    // Ignored unless KICKOS_SCHED_PERIODIC_TICK is opted into; the tickless
    // default arms per event and never reads it.
    constexpr uint64_t KICKOS_TICK_PERIOD_NS = 1_ms;
}

#endif
