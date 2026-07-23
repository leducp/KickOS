// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// User / app provisioning knobs (static-allocation-first, sized per app). The
// integer pool sizes are CMake-`-D` overridable; the ns knob is a typed constexpr.

#ifndef KICKOS_CONFIG_SYSTEM_H
#define KICKOS_CONFIG_SYSTEM_H

#include <stdint.h>

#include <kickos/units.h>

// The selected board's provisioning (MAX_THREADS + the stack sizes) comes from
// board_config.h; CMake puts it on the include path. Absent (sim/standalone),
// the #ifndef defaults below apply -- they are the generous-RAM sim values. A
// CMake -D still overrides.
#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif

// Max registered kernel objects for the M0 sim handle tables.
#ifndef KICKOS_MAX_SEMAPHORES
#define KICKOS_MAX_SEMAPHORES 16
#endif

// Priority-inheritance mutex pool (M3; see cap.h / sync.h). Additive CAP_MUTEX
// object pool, sized like the sems. Tiny 10 KiB boards override to 4.
#ifndef KICKOS_MAX_MUTEXES
#define KICKOS_MAX_MUTEXES 8
#endif

// Endpoint (IPC rendezvous) pool (M3 #4; see cap.h / endpoint.h). Additive
// CAP_ENDPOINT object pool. Default covers console + a service or two; tiny
// boards override. The u8 endpoint_refs assert in instance.h holds either way.
#ifndef KICKOS_MAX_ENDPOINTS
#define KICKOS_MAX_ENDPOINTS 4
#endif

// Static thread pool the syscall thread_spawn draws from (+ its kernel stacks).
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 16
#endif
// Per-task capability-table slots (M3 handle table; see cap.h). Cost is
// MAX_THREADS x MAX_HANDLES x 8 bytes. Indices 0 .. KICKOS_CAP_FIRST_DYNAMIC-1 are the
// FROZEN well-known reserved range (index 0 = kernel stdout; see cap_index.h); own caps
// live in [FIRST_DYNAMIC .. MAX-1]. With FIRST_DYNAMIC=4 the default 12 keeps 8 usable
// own-cap slots (the stress soak's peak / t_mutex_deadlock's 8-cap simultaneity).
// FULL-selftest prerequisite: KICKOS_MAX_HANDLES >= 9. That is FIRST_DYNAMIC(4) + the
// suite's 2 permanent caps (g_done/g_lock) + a 3-own-cap test peak (cap_index0 holds
// sem+endpoint+mutex at once). The four tiny boards floor at exactly 9 so they can run
// it. A board that cannot afford 9 still runs KickOS (real apps use 1-3 caps) but is not
// a full-selftest target: below 9 the selftest hard-fails on cap exhaustion by design.
#ifndef KICKOS_MAX_HANDLES
#define KICKOS_MAX_HANDLES 12
#endif
// Memory-domain pool (the shared region sets threads reference; see domain.h).
// Worst case is one distinct domain per thread plus the two immortal singletons
// (the kernel domain + the default unprivileged domain).
#ifndef KICKOS_MAX_DOMAINS
#define KICKOS_MAX_DOMAINS (KICKOS_MAX_THREADS + 2)
#endif
// Default stack the kernel provides a spawned thread when the caller supplies none --
// so casual use ("just add a task") needs no thought. A thread's stack is a userspace
// concern, so kos_thread_params may instead carry a caller-owned stack_base/stack_size
// to (fine-)tune per thread; the kernel validates it against the floor + alignment below.
#ifndef KICKOS_USER_STACK_SIZE
#define KICKOS_USER_STACK_SIZE (64 * 1024)
#endif
// Floor + alignment for a caller-provided thread stack (must clear the arch context + a
// frame; 16 B suits every ISA's stack ABI). Undersized/misaligned => spawn fails, not a
// silent overflow. The kernel-default and root/idle stacks satisfy these by construction.
#ifndef KICKOS_MIN_STACK_SIZE
#define KICKOS_MIN_STACK_SIZE 512
#endif
#ifndef KICKOS_STACK_ALIGN
#define KICKOS_STACK_ALIGN 16
#endif

// The bootstrap idle/root thread stacks. Provisioning knobs (a syscall runs on
// the calling thread's stack, so root must fit the deepest dispatch). Defaults
// suit the generous-RAM sim; a small-SRAM board overrides them (CMake -D).
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

namespace kickos
{
    // Classic periodic-tick period -- only meaningful in the opt-in
    // KICKOS_SCHED_PERIODIC_TICK build (the tickless default arms per event).
    constexpr uint64_t KICKOS_TICK_PERIOD_NS = 1_ms;
}

#endif
