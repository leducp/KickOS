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
// What this board can BACK: the widest per-task capability table its RAM can spare, and the
// only capability figure a board may state. The width itself is summed from declared demand
// (cmake/cap_table.cmake) and refused if it exceeds this. Each slot costs
// (KICKOS_MAX_THREADS + KCAP_RUN_OFF_POOL) x sizeof(CapEntry) bytes of Kernel .bss.
#ifndef KICKOS_CAP_TABLE_SUPPLY
#define KICKOS_CAP_TABLE_SUPPLY 16
#endif
// Per-task capability-table size. Every task's run is exactly this wide (cap.h carves the
// slab into KICKOS_MAX_HANDLES-entry runs), so raising it costs .bss per possible task.
//
// NOT a knob: cmake/cap_table.cmake sums the four declarations and forwards the total as a
// -D, so a command-line one sets a cache variable nothing reads. The fallback below fires
// only for a compile that has neither the KickOS CMake nor the exported `kickos` target, and
// the assert after it then rejects that compile: the number decides KCAP_RUN_CHUNKS (cap.h),
// hence sizeof(CapRun) and sizeof(Thread), so a TU taking the fallback beside TUs that took
// the summed width disagrees on the Thread layout with nothing to notice.
//
// Indices 0 .. KICKOS_CAP_FIRST_DYNAMIC-1 are the well-known reserved range
// (index 0 = kernel stdout; see cap_index.h); own caps live in [FIRST_DYNAMIC .. MAX-1].
// A child that takes d delegated caps has MAX_HANDLES - 1 - max(d, FIRST_DYNAMIC-1) own
// slots: delegates spend the reserved plane rather than being handed it on top, so an app
// declares for the delegates, not just for the creates.
#ifndef KICKOS_MAX_HANDLES
// Kept as a real number so the assert below is the ONE diagnostic, not the first of a cascade
// of "KICKOS_MAX_HANDLES undeclared" errors out of cap.h and thread.h.
#define KICKOS_MAX_HANDLES 10
#define KICKOS_MAX_HANDLES_IS_FALLBACK 1
#endif
// A static_assert and NOT an #error: cmake/cap_table.cmake and cmake/boot_arena.cmake both
// read this header through `cc -E` before the width exists, and an #error would kill configure
// on every board. A preprocessed probe never reaches semantic analysis, so this is invisible
// to it and hard for anything that actually compiles.
#ifdef KICKOS_MAX_HANDLES_IS_FALLBACK
static_assert(KICKOS_MAX_HANDLES_IS_FALLBACK == 0,
              "KICKOS_MAX_HANDLES came from this header's fallback, not from the "
              "configure-time sum in cmake/cap_table.cmake. It sizes the capability-table "
              "run and picks the chunk geometry, so this TU's sizeof(Thread) need not match "
              "the kernel it links against. Link the exported `kickos` target, which carries "
              "the width as a usage requirement. Do NOT hand-pass it: a wrong value compiles "
              "clean in every TU and nothing here can see it.");
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
