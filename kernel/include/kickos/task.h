// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Tasks: the set of threads that share one memory domain. A task owns a Domain* and counts the
// live threads that joined it; the domain is refcounted by its live TASKS, so it returns to its
// pool when the last task holding it dies.
//
// Three ways in. A plain spawn is pthread_create: the child is a thread of the SPAWNER's task and
// spends no slot here. A spawn bringing its own data grant, or changing privilege, gets an
// IMPLICIT task of its own (task_for). A thread that names a task joins an EXPLICIT one, created
// by task_create before any of its threads exist.
//
// A FAULT is the only death that ends the group from a member's own exit. Every death a caller
// asked for ends one thread; a caller that wants the group calls task_cancel_group itself.
//
// A task owns a DEV window's LIFETIME and never its ACCESS: the window is a region of the asking
// THREAD, and the group cancel is what returns it.

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
        // The kill tag of the thread that called task_create, TRUNCATED to a byte, or
        // KILL_TAG_NONE for an implicit task. Both the creator gate and the reservation: a slot
        // with a creator is not free even at refcount 0, which is what lets an explicit task
        // exist before its first thread. The three bytes here are Domain*'s tail padding.
        uint8_t creator_tag = 0;
        // Bumped when the slot is freed, so a handle to a dead task stops resolving.
        uint16_t gen = 0;
    };

    // 8 bytes on 32-bit, the figure the KICKOS_MAX_TASKS budget is priced against. MEASURE ON A
    // 32-BIT TARGET if this fires: a host build prices the tail differently, so a host
    // measurement cannot settle it.
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

    // Create the IMPLICIT task a new thread belongs to, resolving its domain through domain_for.
    // Reached only where the child cannot be a member of the spawner's group. Takes no reference
    // on either the task or the domain: task_ref, from thread_create, is the commit. A task
    // nobody references is a free SLOT; the domain under it is not free, holding an address
    // space, its tables and a reference on the donor, so a spawn that fails after this point owes
    // task_discard.
    //
    // `caller` is domain_for's posture word (DOM_CALLER_*), resolved by the caller and never read
    // from sched::current() here; `donor` is the domain the grant's memory was reserved in, null
    // where there is none.
    //
    // Returns null on refusal and writes the reason to *err (never null; 0 on success). Every code
    // is domain_for's, forwarded unflattened, plus:
    //   KOS_ENOMEM  the task pool is full.
    Task* task_for(uint32_t caller, void* mem_base, size_t mem_size, Domain* donor,
                   int* err);

    // Create an EXPLICIT task: the group exists, empty, before any thread joins it, and its
    // domain is built from THIS grant. Always unprivileged, whatever the creator is.
    //
    // Takes the domain reference immediately and holds the slot on `creator_tag`'s behalf, so
    // refcount 0 does not free it. Refusals are domain_for's, plus KOS_ENOMEM. mem_attr is
    // domain_for's; task_for passes 0, an implicit task being unable to ask for one.
    Task* task_create(uint16_t creator_tag, uint32_t caller, void* mem_base, size_t mem_size,
                      uint32_t mem_attr, Domain* donor, int* err);

    // Resolve a handle to an EXPLICIT task, or null. An implicit task is unnameable: idle's
    // carries the kernel domain, so a resolvable handle to it would let an unprivileged spawn
    // join the whole arena.
    Task* task_resolve(kos_task_t handle);
    kos_task_t task_handle(Task const* t);
    // The creator gate. False for an implicit task and for KILL_TAG_NONE, so a thread with
    // no spawner matches nobody, exactly as the thread kill gate answers.
    bool task_created_by(Task const* t, uint16_t tag);

    // Release the creator's hold: the group is no longer nameable, and the slot and its
    // domain go back as soon as the last member leaves (at once, if there is none).
    void task_drop_hold(Task* t);

    // Give back a task NOBODY HOLDS: the domain under it, the address space, every frame that
    // space still holds and the reference a handoff took on the donor. Null-safe, and a no-op on
    // a task with a member or a creator hold, whose ends are task_release and task_drop_hold.
    // Compiles to nothing without a translating backend: the call sits on the deepest chain the
    // armv7m and rxv3 trap red zones measure.
#if KICKOS_HAVE_ASPACE
    void task_discard(Task* t);
#else
    inline void task_discard(Task*)
    {
    }
#endif

    // Drop the hold of EVERY task a dying creator holds. Keyed on the tag, which is the whole
    // gate: kill_tag_for_index derives it from the pool slot, so a recycled slot would inherit
    // creator authority over groups it never made. O(1) when Kernel::task_holds is zero;
    // otherwise KICKOS_MAX_TASKS compares interrupt-masked on EVERY thread exit.
    void task_orphan_created_by(uint16_t tag);

    // Live members. Null-safe (0). The ONLY reader outside task.cc is the already-empty
    // early return in kos_task_slay, where a park would never be woken.
    uint8_t task_member_count(Task const* t);

    void task_ref(Task* t); // a thread joins; the first one acquires the domain
    // A thread leaves. For an implicit task the last one out releases the domain and frees the
    // slot, so the caller's Task* is a dangling name from here on. An explicit task outlives its
    // members until its creator drops the hold. TRUE when THIS call emptied the group, which is
    // what a WAIT_TASK_EMPTY waiter is waiting for; the transition happens once and only its
    // cause can see it.
    bool task_release(Task* t);

    // Cancel every live member at `kind` (a CancelKind): each is marked and woken out of
    // whatever it is parked on, so it reaches its own death point. Cooperative at CANCEL_KILL only
    // in that a member which never enters the kernel again never dies; at CANCEL_SLAY every
    // member's resume is claimed instead. The group dies by ONE rule: there is no exception
    // argument, and thread_cancel_kind already refuses a dying thread.
    void task_cancel_group(Task* t, uint8_t kind);
}

#endif
