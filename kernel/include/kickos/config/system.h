// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// User / app provisioning knobs. Every pool size below is CMake-`-D` overridable.

#ifndef KICKOS_CONFIG_SYSTEM_H
#define KICKOS_CONFIG_SYSTEM_H

#include <stdint.h>

#include <kickos/units.h>

// The selected board's provisioning (MAX_THREADS + the stack sizes) comes from
// board_config.h. Absent (sim/standalone), the defaults below apply; they are
// sized for the generous-RAM sim, not for a small-SRAM board.
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

// Static thread pool the syscall thread_spawn draws from (+ its kernel stacks).
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 16
#endif
// Per-task capability-table ceiling, and the width the handle codec's index field is
// derived from. Indices 0 .. KICKOS_CAP_FIRST_DYNAMIC-1 are the well-known reserved
// range (index 0 = kernel stdout; see cap_index.h); own caps live in
// [FIRST_DYNAMIC .. MAX-1].
// A child that takes d delegated caps has MAX_HANDLES - 1 - max(d, FIRST_DYNAMIC-1) own
// slots: delegates spend the reserved plane rather than being handed it on top, so budget
// for the delegates, not just for the creates.
// Below 7 the FULL selftest hard-fails on cap exhaustion by design: it needs
// FIRST_DYNAMIC(2) + 2 permanent caps + a 3-own-cap peak. Lowering the floor on a board
// makes it a non-target for that suite, not a broken kernel.
#ifndef KICKOS_MAX_HANDLES
#define KICKOS_MAX_HANDLES 10
#endif
// Capability-table storage: the size classes the per-task table is taken from at spawn.
// Classes must be ascending, and class 0 is required. A request takes the smallest class
// that fits it and is REFUSED if that class is empty: it never spills into a larger one,
// so a spawn fails iff concurrent same-class demand exceeds that class's static count.
// The default is one class at the full ceiling, one run per possible task, which is
// byte-for-byte and refusal-for-refusal identical to an inline per-TCB table.
#ifndef KICKOS_CAP_CLASS0_SLOTS
#define KICKOS_CAP_CLASS0_SLOTS KICKOS_MAX_HANDLES
#endif
#ifndef KICKOS_CAP_CLASS0_COUNT
#define KICKOS_CAP_CLASS0_COUNT (KICKOS_MAX_THREADS + 2)
#endif
// Classes 1 and 2 are optional: a count of 0 disables the class entirely.
#ifndef KICKOS_CAP_CLASS1_SLOTS
#define KICKOS_CAP_CLASS1_SLOTS 0
#endif
#ifndef KICKOS_CAP_CLASS1_COUNT
#define KICKOS_CAP_CLASS1_COUNT 0
#endif
#ifndef KICKOS_CAP_CLASS2_SLOTS
#define KICKOS_CAP_CLASS2_SLOTS 0
#endif
#ifndef KICKOS_CAP_CLASS2_COUNT
#define KICKOS_CAP_CLASS2_COUNT 0
#endif
// Derived by the root CMakeLists from the class mix and cross-checked against the class
// table in cap.h. Never set by hand; the default exists only so a translation unit built
// outside that scope still compiles.
#ifndef KICKOS_CAP_MULTICLASS
#define KICKOS_CAP_MULTICLASS 0
#endif
// Capacity a spawn that declares none gets.
#ifndef KICKOS_CAP_DEFAULT_CAPACITY
#define KICKOS_CAP_DEFAULT_CAPACITY KICKOS_MAX_HANDLES
#endif

// Caps one spawn may delegate. NOT tied to KICKOS_MAX_HANDLES: thread_spawn stages the
// grant list in CALLER-stack arrays (16 bytes per entry) and root's stack can be 1 KiB,
// so raising this costs caller stack, not .bss, and tying it to the table ceiling would
// turn every ceiling lift into a stack overflow. Grants land at child indices
// 1..cap_count; cap.h static_asserts that they fit.
#ifndef KICKOS_MAX_SPAWN_GRANTS
#define KICKOS_MAX_SPAWN_GRANTS 6
#endif
// Memory-domain pool (see domain.h). Worst case is one distinct domain per thread plus
// the two immortal singletons (the kernel domain + the default unprivileged domain).
#ifndef KICKOS_MAX_DOMAINS
#define KICKOS_MAX_DOMAINS (KICKOS_MAX_THREADS + 2)
#endif
// Stack a spawned thread gets when kos_thread_params carries no caller-owned
// stack_base/stack_size. A caller-supplied stack is validated against the floor and
// alignment below.
#ifndef KICKOS_USER_STACK_SIZE
#define KICKOS_USER_STACK_SIZE (64 * 1024)
#endif
// Floor and alignment for a caller-provided thread stack. The floor is the arch's DEEPEST
// thread-exit dispatch (exit_current -> reschedule -> switch_to -> timer rearm, all on the
// caller's own stack): a stack below it passes the spawn check and then silently overflows
// on exit. The real value is forwarded per arch as a -D from the top-level CMakeLists;
// this default is the conservative MAX across arches, so a build that bypasses that ladder
// is wasteful, never too low. Undersized or misaligned makes the spawn FAIL rather than
// overflow. The idle stack is exempt: it only spins and never runs the exit dispatch.
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

namespace kickos
{
    // Ignored unless KICKOS_SCHED_PERIODIC_TICK is opted into; the tickless
    // default arms per event and never reads it.
    constexpr uint64_t KICKOS_TICK_PERIOD_NS = 1_ms;
}

#endif
