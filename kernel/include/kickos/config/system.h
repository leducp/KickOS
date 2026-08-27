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

// Threads the syscall thread_spawn can seat CONCURRENTLY (+ their kernel stacks). This is
// what every defconfig states and what the boot arena backs one default stack per; it does
// NOT count root, which holds a slot of its own.
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 16
#endif
// Slots in the thread pool (thread.h). Root takes one at boot and never reaches EXITED, so
// it is never reclaimed and a spawn can still draw KICKOS_MAX_THREADS of them. Size a TCB
// array or a cap-run count from THIS; size a user stack or the boot arena from
// KICKOS_MAX_THREADS.
#define KICKOS_THREAD_SLOTS (KICKOS_MAX_THREADS + 1)
// What this board can BACK: the widest per-thread capability table its RAM can spare, and
// the only capability figure a board may state. The width itself is summed from declared demand
// (cmake/cap_table.cmake) and refused if it exceeds this. It prices ROOT's table alone: the
// slab backs one CHILD-width run per holder plus root's own widening (KCAP_SLAB_CHUNKS,
// cap.h), so a slot here does not cost (KICKOS_THREAD_SLOTS + KCAP_RUN_OFF_POOL) of itself.
#ifndef KICKOS_CAP_TABLE_SUPPLY
#define KICKOS_CAP_TABLE_SUPPLY 16
#endif
// Caps one spawn may delegate. NOT tied to KICKOS_MAX_HANDLES: thread_spawn stages the
// grant list in CALLER-stack arrays (16 bytes per entry) and root's stack can be 1 KiB,
// so raising this costs caller stack, not .bss, and tying it to the table ceiling would
// turn every ceiling lift into a stack overflow. Grants land at child indices
// 1..cap_count; cap.h static_asserts that they fit.
#ifndef KICKOS_MAX_SPAWN_GRANTS
#define KICKOS_MAX_SPAWN_GRANTS 6
#endif
// Virtual ranges one address space can name (see vrange.h): its process image, its stacks
// and whatever it has reserved. A page table has no describable-extent ceiling, so unlike
// KICKOS_MPU_MAX_REGIONS this bounds the VALIDATION list alone and buys back none of what a
// descriptor budget cost.
#ifndef KICKOS_ASPACE_RANGES
#define KICKOS_ASPACE_RANGES 8
#endif
// Task pool (see task.h). One task per LIVE THREAD, since grouping is implicit today, so
// the bound is every TCB that can exist at once: KICKOS_THREAD_SLOTS (root + the threads
// a spawn may seat) plus idle, which holds a TCB outside the pool. There is no immortal
// task, so this needs neither of the two extra slots KICKOS_MAX_DOMAINS spends on its
// singletons. A slot short and task_for would refuse a spawn the thread pool would still
// have seated (task.cc static_asserts the floor).
#ifndef KICKOS_MAX_TASKS
#define KICKOS_MAX_TASKS (KICKOS_THREAD_SLOTS + 1)
#endif
// Memory-domain pool (see domain.h), plus the two immortal singletons (the kernel domain
// and the default unprivileged one).
//
// THE TWO BACKENDS COUNT DIFFERENTLY, and it is a count of DOMAINS: nothing here is paid
// out of KICKOS_MPU_MAX_REGIONS, which bounds how much ONE domain describes. A region
// backend reaches at most one distinct domain per thread, every no-grant task resolving to
// the shared singleton. A translating one spends one per TASK instead: a domain carries an
// address space there, so no two tasks may share one and the singleton is a template
// rather than a domain to join (docs/design-m6-mmu.md F2).
#ifndef KICKOS_MAX_DOMAINS
#if KICKOS_HAVE_ASPACE
#define KICKOS_MAX_DOMAINS (KICKOS_MAX_TASKS + 2)
#else
#define KICKOS_MAX_DOMAINS (KICKOS_MAX_THREADS + 2)
#endif
#endif
// Independent kernels co-resident in ONE address space (instance.h). One on a chip, and
// one per emulated MCU under the multi-instance sim. It is a provisioning bound, not a
// preference: every instance-scoped object is this many copies, so raising it costs .bss
// linearly. At 1 the index folds to a literal and the image carries none of it.
#ifndef KICKOS_MAX_INSTANCES
#define KICKOS_MAX_INSTANCES 1
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
