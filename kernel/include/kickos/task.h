// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Tasks: the set of threads that share one memory domain. A task owns a Domain*
// and counts the live threads that joined it; the domain is refcounted by its
// live TASKS, not directly by threads, so a domain returns to its pool when the
// last task holding it dies.
//
// Two ways in. A thread that names no task gets an IMPLICIT task holding exactly
// itself, so every spawn written before this layer existed means what it always
// meant. A thread that names one joins an EXPLICIT task, created by task_create
// before any of its threads exist, and is then coupled to its peers: the group is
// killed as a unit (task_cancel_group) and any member's death ends the rest.
//
// A task owns a DEV window's LIFETIME and never its ACCESS: the window is a region
// of the asking THREAD (thread.cc), so a peer that did not ask for it cannot reach
// the registers, and task-scoped death is what returns the window when the group
// goes. See docs/design-task-layer.md sections 5.2 and 6.

#ifndef KICKOS_TASK_H
#define KICKOS_TASK_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/config.h>

#include <kickos/sys/abi.h> // kos_task_t, KOS_TASK_NONE

namespace kickos
{
    struct Domain; // kickos/domain.h: the shared region set this task's threads see
    struct Thread; // kickos/thread.h: a member

    struct Task
    {
        // Read from outside task.cc through the accessors below only. EVERY member must
        // stay ZERO-initialised: a non-zero initialiser anywhere in Kernel moves the whole
        // constinit instance out of .bss and into .data.
        Domain* domain = nullptr;
        uint8_t refcount = 0; // live threads
        // The kill tag of the thread that called task_create, TRUNCATED to a byte (task.cc
        // asserts the pool cannot alias one tag onto another), or KILL_TAG_NONE for an
        // implicit task. It is BOTH the creator gate and the reservation: a slot with a
        // creator is not free even at refcount 0, which is what lets an explicit task exist
        // before its first thread. The three bytes here are the tail padding Domain* leaves,
        // so none of them grows the pool.
        uint8_t creator_tag = 0;
        // Bumped when the slot is freed, so a handle to a dead task stops resolving.
        uint16_t gen = 0;
    };

    // 8 bytes on 32-bit, which design-task-layer.md, invariants.md and porting.md all
    // budget against. MEASURE ON A 32-BIT TARGET if this fires: a host build prices the
    // tail differently, so a host measurement cannot settle it.
    constexpr size_t task_scalar_bytes()
    {
        size_t const raw = sizeof(Domain*) + sizeof(Task::refcount) + sizeof(Task::creator_tag)
                          + sizeof(Task::gen);
        return ((raw + sizeof(void*) - 1) / sizeof(void*)) * sizeof(void*);
    }

    constexpr size_t KICKOS_TASK_EXPECTED_SIZE = task_scalar_bytes();

    static_assert(sizeof(Task) == KICKOS_TASK_EXPECTED_SIZE,
                  "sizeof(Task) moved. Either drop the member that grew it, or re-measure on a "
                  "32-BIT target and edit task_scalar_bytes: a host measurement prices this "
                  "differently from a 32-bit target");
    // No no-tail-padding assert to match Thread's: Domain*'s alignment leaves 4 bytes after
    // `gen` on the host and none on 32-bit, so such an assert cannot hold on both.

    // Handle codec. The index is stored BIASED BY ONE so that no live task is ever named by
    // the all-zero word: KOS_TASK_NONE is 0, and kos_thread_params zero-initialised by an
    // app that predates this field must mean "no task".
    constexpr int TASK_INDEX_BITS = 16;

    // Boot: clear the pool. Reads nothing from the domain pool, so it carries no ordering
    // constraint against domain_init.
    void task_init(void);

    // The domain this task's threads share, or null for a task holding nothing.
    // Null-safe.
    Domain* task_domain(Task const* t);

    // Create the IMPLICIT task a new thread belongs to, resolving its domain through
    // domain_for and wrapping the result. Takes no reference on either the task or
    // the domain: task_ref, from thread_create, is the commit. A task nobody
    // references stays at refcount 0 with no creator, which is a free slot, so a spawn
    // that fails after this point leaves nothing to unwind.
    //
    // caller_authorized is domain_for's, with domain_for's meaning: the SPAWNER's
    // AUTH_MEMORY answer, resolved by the caller and never read from
    // sched::current() here.
    //
    // Returns null on refusal and writes the reason to *err (never null; 0 on
    // success). Every code is domain_for's, forwarded unflattened, plus:
    //   KOS_ENOMEM  the task pool is full.
    // That arm is unreachable while KICKOS_MAX_TASKS covers every TCB that can be
    // live at once (the static_assert in task.cc), which is what makes the
    // -KOS_ENOMEM a thread-pool exhaustion already returns stay the first refusal
    // a caller sees.
    Task* task_for(bool privileged, void* mem_base, size_t mem_size,
                   bool caller_authorized, int* err);

    // Create an EXPLICIT task: the group exists, empty, before any thread joins it, and its
    // domain is built from THIS grant rather than from whatever the first thread happened to
    // ask for. Always unprivileged, whatever the creator is: a privileged thread holds the
    // kernel domain and has nothing to share.
    //
    // Takes the domain reference immediately and holds the slot on `creator_tag`'s behalf,
    // so refcount 0 does not free it. Refusals are domain_for's, plus KOS_ENOMEM.
    Task* task_create(uint16_t creator_tag, void* mem_base, size_t mem_size,
                      bool caller_authorized, int* err);

    // Resolve a handle to an EXPLICIT task, or null. An implicit task is deliberately
    // unnameable: idle's and root's carry the kernel domain, and a resolvable handle to one
    // would let an unprivileged spawn join the whole arena.
    Task* task_resolve(kos_task_t handle);
    kos_task_t task_handle(Task const* t);
    // The creator gate. False for an implicit task and for KILL_TAG_NONE, so a thread with
    // no spawner matches nobody, exactly as the thread kill gate answers.
    bool task_created_by(Task const* t, uint16_t tag);

    // Release the creator's hold: the group is no longer nameable, and the slot and its
    // domain go back as soon as the last member leaves (at once, if there is none).
    void task_drop_hold(Task* t);

    void task_ref(Task* t); // a thread joins; the first one acquires the domain
    // A thread leaves. For an implicit task the last one out releases the domain and frees
    // the slot, so the caller's Task* is a dangling name from here on, exactly as its
    // Domain* was. An explicit task outlives its members until its creator drops the hold.
    void task_release(Task* t);

    // Cancel every live member: each is marked and woken out of whatever it is parked on, so
    // it reaches its own death point (thread_cancel, kernel.h). Cooperative only in that a
    // member which never enters the kernel again never dies.
    //
    // Takes no "except" argument, and must not grow one: the caller that has a thread to spare
    // is exit_current, whose thread is `dying` by then, and thread_cancel already refuses a
    // dying thread. A second guard for the same fact would be the one that goes stale.
    void task_cancel_group(Task* t);
}

#endif
