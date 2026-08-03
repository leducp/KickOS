// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Capability-table manager (see cap.h): the per-task naming+rights layer over the
// global object pools, plus the object-side refcount (kernel().sem_refs) that owns
// destroy-on-last-close. slotpool.h stays generic: refs[] lives here.

#include <kickos/cap.h>
#include <kickos/console_tx.h> // console_note_driver_death
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>

#include <kickos/sys/errno.h>

#include <string.h>

namespace kickos
{
    namespace
    {
        // Console stdout target: the GLOBAL gen-encoded endpoint handle a userspace
        // console driver serves, or -1 pre-publish. The kernel holds ONE ref on it
        // (moved on re-publish); cap_install_defaults seats a send-only copy at index 0
        // of every child. See docs/design-m3-console-handover-stageii.md (D3/D4/S3).
        int g_stdout_target = -1;

        // Threads inside cap_teardown right now. A count, not a flag: an RR slice expiring
        // in sched::tick_rr can switch a dying thread out at a chunk boundary and a second
        // thread can then enter and finish its own sweep first. Gates the console reclaim,
        // which must not run while ANY dying thread might still hold an IRQ cap on the line.
        unsigned g_teardown_depth = 0;

        // One flat .bss array carved into fixed-class runs at boot. A class's free list is
        // threaded THROUGH THE FREE RUNS THEMSELVES, so there is no side table; the assert
        // below is what keeps an entry wide enough to hold the link.
        CapEntry g_cap_slab[kcap_slab_entries()];
        CapEntry* g_cap_free[KCAP_CLASSES_LIVE] = {};

        static_assert(sizeof(CapEntry) >= sizeof(void*),
                      "a free run must be able to hold its own free-list link");

        CapEntry** run_link(CapEntry* run) { return reinterpret_cast<CapEntry**>(run); }

        // Slot index of the semaphore a global handle names (via the live object, so
        // the SlotPool handle codec is never assumed here). -1 if it does not resolve.
        int sem_index_of(int obj_handle)
        {
            Semaphore* s = kernel().sems.resolve(obj_handle);
            if (s == nullptr)
            {
                return -1;
            }
            return static_cast<int>(s - kernel().sems.at(0));
        }

        // Drop one reference to semaphore `obj_handle`; free it at refs -> 0. `teardown` is
        // the noreturn exit path, which must never strand a parked waiter, so a would-be free
        // with waiters still linked LEAKS (floors refs at 1). That branch is unreachable via
        // close, since a parked waiter pins its own cap, hence the assert.
        void sem_ref_drop(int obj_handle, bool teardown)
        {
            int const idx = sem_index_of(obj_handle);
            if (idx < 0)
            {
                return; // already gone: cannot happen under correct refcounting
            }
            uint8_t& r = kernel().sem_refs[idx];
            if (r > 0)
            {
                r--;
            }
            if (r == 0)
            {
                Semaphore* s = kernel().sems.resolve(obj_handle);
                if (s != nullptr and not s->waiters.empty())
                {
                    KICKOS_ASSERT(teardown); // refs->0 with a waiter parked is unreachable via close
                    r = 1;                   // leak, never strand
                    return;
                }
                kernel().sems.free(obj_handle);
            }
        }

        // Slot index of the mutex a global handle names (via the live object, as with
        // sems). -1 if it does not resolve.
        int mutex_index_of(int obj_handle)
        {
            Mutex* m = kernel().mutexes.resolve(obj_handle);
            if (m == nullptr)
            {
                return -1;
            }
            return static_cast<int>(m - kernel().mutexes.at(0));
        }

        // Drop one reference to mutex `obj_handle`; free at refs -> 0. Same leak-don't-strand
        // guard as sem_ref_drop: refs -> 0 with a waiter still parked is unreachable via close,
        // because a parked waiter is BLOCKED and cannot run handle_close. R4: refs -> 0 also
        // implies owner == nullptr, since an owner's own cap pins a ref via the R2 close guard
        // and R3 force-unlocked before this drop on the exit path.
        void mutex_ref_drop(int obj_handle, bool teardown)
        {
            int const idx = mutex_index_of(obj_handle);
            if (idx < 0)
            {
                return;
            }
            uint8_t& r = kernel().mutex_refs[idx];
            if (r > 0)
            {
                r--;
            }
            if (r == 0)
            {
                Mutex* m = kernel().mutexes.resolve(obj_handle);
                if (m != nullptr and not m->waiters.empty())
                {
                    KICKOS_ASSERT(teardown); // refs->0 with a waiter parked is unreachable via close
                    r = 1;                   // leak, never strand
                    return;
                }
                KICKOS_ASSERT(m == nullptr or m->owner == nullptr); // R4: never free a locked, reachable mutex
                kernel().mutexes.free(obj_handle);
            }
        }

        // Slot index of the endpoint a global handle names (via the live object, as
        // with sems/mutexes). -1 if it does not resolve.
        int endpoint_index_of(int obj_handle)
        {
            Endpoint* e = kernel().endpoints.resolve(obj_handle);
            if (e == nullptr)
            {
                return -1;
            }
            return static_cast<int>(e - kernel().endpoints.at(0));
        }

        // Drop one reference to endpoint `obj_handle`; free at refs -> 0. The
        // leak-don't-strand guard checks BOTH waitqs, and is unreachable via close: a parked
        // sender pins its own SIGNAL cap and a parked receiver its own WAIT cap, recv_holders
        // -> 0 has already emptied send_waiters, and recv_waiters requires a WAIT cap.
        void endpoint_ref_drop(int obj_handle, bool teardown)
        {
            int const idx = endpoint_index_of(obj_handle);
            if (idx < 0)
            {
                return;
            }
            uint8_t& r = kernel().endpoint_refs[idx];
            if (r > 0)
            {
                r--;
            }
            if (r == 0)
            {
                Endpoint* e = kernel().endpoints.resolve(obj_handle);
                if (e != nullptr and (not e->send_waiters.empty() or not e->recv_waiters.empty()))
                {
                    KICKOS_ASSERT(teardown); // refs->0 with a waiter parked is unreachable via close
                    r = 1;                   // leak, never strand
                    return;
                }
                kernel().endpoints.free(obj_handle);
            }
        }

        // Drop one reference to the object a (now-detached) cap entry named. A new type
        // reaching the default without its own arm traps in debug and leaks in release; a
        // silent skip would lose the reference with no diagnostic.
        void obj_ref_drop(CapEntry const& e, bool teardown)
        {
            switch (static_cast<CapType>(e.type))
            {
            case CapType::CAP_SEM:
            {
                sem_ref_drop(e.obj, teardown);
                return;
            }
            case CapType::CAP_MUTEX:
            {
                mutex_ref_drop(e.obj, teardown);
                return;
            }
            case CapType::CAP_ENDPOINT:
            {
                endpoint_ref_drop(e.obj, teardown);
                return;
            }
            case CapType::CAP_REPLY:
            {
                return; // names a thread by generational handle: holds no pool refcount
            }
            case CapType::CAP_IRQ:
            {
                irq_ref_drop(e.obj, teardown);
                return;
            }
            default:
            {
                KICKOS_ASSERT(false);
                return;
            }
            }
        }

        // Per-type close/exit protocol, run BEFORE detach + drop at both call sites.
        // Returns 0, or a negative -KOS_E* to refuse a voluntary (non-teardown) close.
        int obj_close_protocol(Thread* closer, CapEntry const& e, bool teardown)
        {
            switch (static_cast<CapType>(e.type))
            {
            case CapType::CAP_SEM:
            {
                return 0;
            }
            case CapType::CAP_MUTEX:
            {
                Mutex* m = kernel().mutexes.resolve(e.obj);
                if (m == nullptr or m->owner != closer)
                {
                    return 0; // not the owner: an ordinary refcount close, no protocol
                }
                if (not teardown)
                {
                    return -KOS_EBUSY; // R2: refuse a voluntary close of a mutex you OWN (unlock first)
                }
                // R3: the owner is exiting. Force-unlock BEFORE the ref drop so a
                // waiter is never stranded; the woken lock() caller gets OWNER_DIED.
                mutex_force_unlock(m, closer);
                return 0;
            }
            case CapType::CAP_ENDPOINT:
            {
                // #4: dropping the LAST WAIT-bearing cap makes the endpoint dead (no
                // receiver can ever exist), so EPIPE every parked sender. Fired exactly
                // once (recv_holders -> 0), on BOTH voluntary close and exit teardown.
                Endpoint* ep = kernel().endpoints.resolve(e.obj);
                if (ep != nullptr and (e.rights & CAP_WAIT) != 0)
                {
                    // B2: this closer was the conventional server: drop the dangling
                    // pointer (else a later D2 boost writes a reused TCB) and kill any
                    // lingering D2 donation. A dying closer is never rescheduled, so it
                    // skips its own recompute (mirrors mutex_force_unlock).
                    if (ep->server == closer)
                    {
                        ep->server = nullptr;
                        // A live closer self-lowers: give up the CPU if a higher thread is
                        // now the top runnable (H8), mirroring mutex_unlock's no-waiter path.
                        if (not teardown)
                        {
                            uint8_t const np = thread_effective_prio(closer);
                            if (np != closer->prio)
                            {
                                sched::set_prio(closer, np);
                                sched::reschedule();
                            }
                        }
                    }
                    if (ep->recv_holders > 0)
                    {
                        ep->recv_holders--;
                        if (ep->recv_holders == 0)
                        {
                            Thread* s;
                            while ((s = wq_pop_highest(ep->send_waiters)) != nullptr)
                            {
                                // last receiver gone: EPIPE the parked sender. A SEND_WAIT
                                // caller returns via kos_call's B1 call_state clear.
                                s->wait_result = -KOS_EPIPE;
                                sched::wake(s);
                            }
                            // If that endpoint was the published console, no userspace
                            // driver can ever serve it again: NOTE it, and let
                            // exit_current run the reclaim after the whole teardown loop
                            // (console_tx.h explains why not here).
                            //
                            // The note is NOT the reclaim decision. recv_holders counts
                            // WAIT-bearing caps, so on a two-thread driver it reaches 0 when
                            // the SERVICE thread dies, while the registers belong to the IRQ
                            // thread, which parks on a line cap and is not counted here at
                            // all. console_on_driver_death asks the device separately and
                            // defers while any live domain still holds the register window.
                            if (e.obj == g_stdout_target)
                            {
                                console_note_driver_death();
                            }
                        }
                    }
                }
                return 0; // endpoints NEVER refuse a close (unlike mutex R2)
            }
            case CapType::CAP_IRQ:
            {
                // Deliberately EMPTY, and it must STAY empty: the endpoint arm's EPIPE-wake
                // has no reachable analogue here, because a parked irq_wait waiter always
                // holds its own cap (waiters <= refs) and cancellation unlinks the target
                // before the target's own teardown runs. Only an ASYNCHRONOUS destroy could
                // make the case reachable, and the leak-never-strand guard in irq_ref_drop
                // is the assert that would say so.
                return 0;
            }
            case CapType::CAP_REPLY:
            {
                // m9: run the SAME full stale-resolve as kos_reply before waking. Fires on
                // both voluntary close of a reply cap AND server-death teardown. If the
                // caller is still parked, EPIPE it; the one-shot consume (empty + gen bump)
                // happens at the shared close/teardown site after this returns. A dying
                // closer skips its recompute: it has only the rest of its own sweep left.
                Thread* caller = cap_reply_caller(e.obj);
                if (caller != nullptr)
                {
                    caller->call_state = CALL_NONE; // stop the funnel counting this donor
                    reply_donor_unpark(closer, caller); // and take it off the donor list
                    caller->wait_result = -KOS_EPIPE;
                }
                // Deflate BEFORE waking (H8): the wake's reschedule must run against our
                // reverted priority, else the woken high-prio caller cannot preempt the
                // still-boosted closer. Mirrors endpoint_reply's deflate-then-wake order.
                if (not teardown)
                {
                    sched::set_prio(closer, thread_effective_prio(closer));
                }
                if (caller != nullptr)
                {
                    sched::wake(caller);
                }
                return 0;
            }
            default:
            {
                return 0;
            }
            }
        }
    }

    namespace
    {
        // Locate every counter one cap naming `obj_handle` moves: the object-side refcount,
        // plus recv_holders for a WAIT-bearing endpoint cap. False = nothing to move (a
        // poolless type, or a handle that no longer resolves). ONE locator serves both
        // directions, so obj_ref_inc and obj_ref_undo cannot drift apart and cannot move one
        // endpoint counter without the other.
        bool ref_counters(CapType type, int obj_handle, uint8_t rights,
                          uint8_t** refs, uint8_t** holders)
        {
            *refs = nullptr;
            *holders = nullptr;
            switch (type)
            {
            case CapType::CAP_SEM:
            {
                int const idx = sem_index_of(obj_handle); // rights: sem accounting ignores them
                if (idx < 0)
                {
                    return false;
                }
                *refs = &kernel().sem_refs[idx];
                return true;
            }
            case CapType::CAP_MUTEX:
            {
                int const idx = mutex_index_of(obj_handle);
                if (idx < 0)
                {
                    return false;
                }
                *refs = &kernel().mutex_refs[idx];
                return true;
            }
            case CapType::CAP_ENDPOINT:
            {
                int const idx = endpoint_index_of(obj_handle);
                if (idx < 0)
                {
                    return false;
                }
                *refs = &kernel().endpoint_refs[idx];
                // A cap COPY carrying CAP_WAIT adds a receiver holder.
                if ((rights & CAP_WAIT) != 0)
                {
                    *holders = &kernel().endpoints.at(idx)->recv_holders;
                }
                return true;
            }
            case CapType::CAP_IRQ:
            {
                IrqBinding* b = kernel().irq_bindings.resolve(obj_handle);
                if (b == nullptr)
                {
                    return false;
                }
                *refs = &kernel().irq_refs[static_cast<int>(b - kernel().irq_bindings.at(0))];
                return true;
            }
            case CapType::CAP_REPLY:
            {
                return false; // names a thread by generational handle: no pool refcount
            }
            default:
            {
                KICKOS_ASSERT(false); // unknown type must trap in debug
                return false;
            }
            }
        }
    }

    // Bump one reference to the object a global handle names. Handle MUST resolve.
    // Caller holds IrqLock.
    bool obj_ref_inc(CapType type, int obj_handle, uint8_t rights)
    {
        uint8_t* refs = nullptr;
        uint8_t* holders = nullptr;
        if (not ref_counters(type, obj_handle, rights, &refs, &holders))
        {
            return true; // nothing to bump: not a refusal
        }
        // BOTH ceilings are tested before EITHER counter moves: an endpoint left with
        // endpoint_refs bumped and recv_holders not (or the reverse) would leak a
        // receiver into the dead-endpoint gate that no close could ever take back.
        if (*refs == UINT8_MAX or (holders != nullptr and *holders == UINT8_MAX))
        {
            return false;
        }
        (*refs)++;
        if (holders != nullptr)
        {
            (*holders)++;
        }
        return true;
    }

    void obj_ref_undo(CapType type, int obj_handle, uint8_t rights)
    {
        uint8_t* refs = nullptr;
        uint8_t* holders = nullptr;
        if (not ref_counters(type, obj_handle, rights, &refs, &holders))
        {
            return;
        }
        // Never reaches 0: every caller undoes a bump taken on an object some LIVE cap
        // already named, so that cap holds the last reference. Hence no free-at-zero arm
        // and no close protocol here.
        KICKOS_ASSERT(*refs > 1);
        if (*refs > 0)
        {
            (*refs)--;
        }
        if (holders != nullptr and *holders > 0)
        {
            (*holders)--;
        }
    }

    CapEntry* cap_lookup(Thread* c, int cap_handle)
    {
        if (c == nullptr)
        {
            return nullptr;
        }
        int const idx = cap_handle & ((1 << KCAP_INDEX_BITS) - 1);
        // Bounded by THIS task's run, not by the largest class: a handle whose index is
        // encodable but past the run this task was given must fail to resolve, never read
        // a neighbouring task's run off the end of its own.
        if (c->handles == nullptr or idx >= static_cast<int>(c->cap_capacity))
        {
            return nullptr;
        }
        CapEntry& e = c->handles[idx];
        if (e.type == static_cast<uint8_t>(CapType::CAP_EMPTY))
        {
            return nullptr;
        }
        // Full high bits (not truncated): a handle carrying junk above the cap-gen field
        // must fail to resolve, not alias (mirrors slotpool.h's resolve()).
        uint32_t const cgen = static_cast<uint32_t>(cap_handle) >> KCAP_INDEX_BITS;
        if (static_cast<uint32_t>(e.gen) != cgen)
        {
            return nullptr;
        }
        return &e;
    }

    void* cap_resolve_e(Thread* c, int cap_handle, CapType want, uint8_t need, int* err)
    {
        *err = KOS_EBADF; // bad index / empty / stale cap-gen / wrong type / stale object
        CapEntry* e = cap_lookup(c, cap_handle);
        if (e == nullptr)
        {
            return nullptr;
        }
        if (e->type != static_cast<uint8_t>(want))
        {
            return nullptr;
        }
        if ((e->rights & need) != need) // rights enforced HERE, nowhere else
        {
            *err = KOS_EPERM; // named a valid cap but it lacks a required right
            return nullptr;
        }
        // WRAP: the stored global handle re-checks the object-gen in its own pool. A
        // stale object (freed under a still-live cap) stays EBADF (set above).
        void* p = nullptr;
        if (want == CapType::CAP_SEM)
        {
            p = kernel().sems.resolve(e->obj);
        }
        else if (want == CapType::CAP_MUTEX)
        {
            p = kernel().mutexes.resolve(e->obj);
        }
        else if (want == CapType::CAP_ENDPOINT)
        {
            p = kernel().endpoints.resolve(e->obj);
        }
        else if (want == CapType::CAP_IRQ)
        {
            p = kernel().irq_bindings.resolve(e->obj);
        }
        if (p != nullptr)
        {
            *err = 0;
        }
        return p;
    }

    void* cap_resolve(Thread* c, int cap_handle, CapType want, uint8_t need)
    {
        int err = 0;
        return cap_resolve_e(c, cap_handle, want, need, &err);
    }

    bool cap_check_authority(Thread* c, uint8_t need)
    {
        if (c == nullptr)
        {
            return false; // no caller context: refuse rather than assume the kernel
        }
        if (c->privileged)
        {
            return true; // privileged implies every authority
        }
        return (c->authority & need) == need;
    }

    void cap_seat_authority(Thread* t, uint8_t auth)
    {
        // Mask to AUTH_* bits, so a caller cannot seat a bit no gate reads.
        t->authority = static_cast<uint8_t>(auth & CAP_AUTH_ALL);
    }

    int cap_narrow_authority(Thread* c, int cap_handle, uint8_t mask)
    {
        if (cap_handle != KOS_CAP_AUTHORITY)
        {
            // Object caps are out of scope: dropping CAP_WAIT from an endpoint cap would
            // have to run the recv_holders accounting obj_close_protocol does.
            return -KOS_EINVAL;
        }
        if (c->authority == 0)
        {
            // Nothing to give up. A privileged thread lands here too: its permission does
            // not come from this word, so narrowing it would be a lie.
            return -KOS_EBADF;
        }
        c->authority = static_cast<uint8_t>(c->authority & mask); // & can only clear bits
        return 0;
    }

    void cap_slab_init()
    {
        uint32_t off = 0;
        for (int i = 0; i < KCAP_CLASSES_LIVE; i++)
        {
            g_cap_free[i] = nullptr;
            // Push in reverse so the list comes out in address order and a first attach is
            // deterministic across boots.
            for (int r = KCAP_CLASSES[i].count - 1; r >= 0; r--)
            {
                CapEntry* run = &g_cap_slab[off + static_cast<uint32_t>(r) * KCAP_CLASSES[i].slots];
                *run_link(run) = g_cap_free[i];
                g_cap_free[i] = run;
            }
            off += static_cast<uint32_t>(KCAP_CLASSES[i].slots)
                   * static_cast<uint32_t>(KCAP_CLASSES[i].count);
        }
        KICKOS_ASSERT(off == kcap_slab_entries());
    }

    CapEntry* cap_slab_attach(uint16_t want, uint8_t* cls, uint16_t* capacity)
    {
        for (int i = 0; i < KCAP_CLASSES_LIVE; i++)
        {
            if (KCAP_CLASSES[i].slots < want)
            {
                continue;
            }
            // The FIRST fitting class and no other: spilling into a larger one would let
            // small spawns eat the runs a big task depends on.
            CapEntry* run = g_cap_free[i];
            if (run == nullptr)
            {
                return nullptr;
            }
            g_cap_free[i] = *run_link(run);
            // CAP_EMPTY is 0 and so is a fresh cap-gen, so a zeroed run is an empty table.
            memset(run, 0, static_cast<size_t>(KCAP_CLASSES[i].slots) * sizeof(CapEntry));
            *cls = static_cast<uint8_t>(i);
            *capacity = KCAP_CLASSES[i].slots;
            return run;
        }
        return nullptr;
    }

    void cap_slab_detach(CapEntry* run, uint8_t cls)
    {
        if (run == nullptr)
        {
            return;
        }
        KICKOS_ASSERT(cls < KCAP_CLASSES_LIVE);
        *run_link(run) = g_cap_free[cls];
        g_cap_free[cls] = run;
    }

    void cap_install_at(Thread* c, int index, int obj_handle, CapType type, uint8_t rights)
    {
        // Index 0 is the kernel stdout slot, written directly by cap_seat_stdout and
        // cap_install_defaults, so this entry point rejects it outright: no delegation or
        // own-create may alias stdout. An out-of-range index is a kernel bug, so it traps in
        // debug and no-ops in release rather than scribble another thread's slot.
        if (index <= KOS_CAP_STDOUT or c->handles == nullptr
            or index >= static_cast<int>(c->cap_capacity))
        {
            KICKOS_ASSERT(false);
            return;
        }
        CapEntry& e = c->handles[index];
        e.obj = obj_handle;
        e.type = static_cast<uint8_t>(type);
        e.rights = rights;
    }

    int cap_install(Thread* c, int obj_handle, CapType type, uint8_t rights)
    {
        // The scan starts at KICKOS_CAP_FIRST_DYNAMIC so an own create can NEVER land on a
        // reserved well-known index. Reserved slots are seated only by the kernel (stdout) or
        // by explicit spawn delegation; see <kickos/sys/cap_index.h>.
        int const cap_end = static_cast<int>(c->cap_capacity);
        for (int i = KICKOS_CAP_FIRST_DYNAMIC; i < cap_end; i++)
        {
            if (c->handles[i].type == static_cast<uint8_t>(CapType::CAP_EMPTY))
            {
                cap_install_at(c, i, obj_handle, type, rights);
                return static_cast<int>((static_cast<uint32_t>(c->handles[i].gen) << KCAP_INDEX_BITS)
                                        | static_cast<uint32_t>(i));
            }
        }
        return -1;
    }

    bool cap_has_free_slot(Thread* c)
    {
        int const cap_end = static_cast<int>(c->cap_capacity);
        for (int i = KICKOS_CAP_FIRST_DYNAMIC; i < cap_end; i++)
        {
            if (c->handles[i].type == static_cast<uint8_t>(CapType::CAP_EMPTY))
            {
                return true;
            }
        }
        return false;
    }

    Thread* cap_reply_caller(int32_t obj)
    {
        // MASKED shifts (m6): the seq8 top bit makes obj negative, so an arithmetic
        // shift would corrupt the packed thread handle.
        uint32_t const u = static_cast<uint32_t>(obj);
        int const handle = static_cast<int>(u & 0xFFFFFFu);
        uint8_t const seq8 = static_cast<uint8_t>(u >> 24);
        int const index = handle & ((1 << ThreadPool::INDEX_BITS) - 1);
        uint32_t const gen =
            (static_cast<uint32_t>(handle) >> ThreadPool::INDEX_BITS) & 0xFFFFu;
        ThreadPool& tp = kernel().threads;
        if (index < 0 or index >= tp.next)
        {
            return nullptr;
        }
        if (static_cast<uint32_t>(tp.gen[index]) != gen)
        {
            return nullptr; // slot reclaimed under the cap: stale
        }
        Thread* t = &tp.slots[index];
        if (t->state != ThreadState::BLOCKED or t->call_state != CALL_REPLY_WAIT)
        {
            return nullptr; // not parked in a call anymore (replied / aborted)
        }
        if (static_cast<uint8_t>(t->call_seq & 0xFF) != seq8)
        {
            return nullptr; // a newer call rolled the seq (late-reply ABA guard)
        }
        return t;
    }

    int handle_close(Thread* c, int cap_handle)
    {
        CapEntry* e = cap_lookup(c, cap_handle);
        if (e == nullptr)
        {
            return -KOS_EBADF;
        }
        int const refused = obj_close_protocol(c, *e, /*teardown=*/false);
        if (refused != 0)
        {
            return refused; // protocol refused the close (#3: owner closing a held mutex -> -KOS_EBUSY)
        }
        CapEntry const detached = *e;
        // Stale the handle + empty the slot BEFORE dropping the ref, so the slot is
        // cleanly reusable and no stale handle resolves during the drop.
        e->gen++;
        e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
        e->obj = 0;
        e->rights = 0;
        obj_ref_drop(detached, /*teardown=*/false);
        // A VOLUNTARY close can also be the published console's last receiver going away, so
        // the reclaim runs HERE and not as a note for whichever thread exits next: there is
        // no teardown loop to finish first, and a pending note would reclaim the console at
        // an unrelated moment. A no-op unless the note is set.
        //
        // Skipped while a teardown sweep is in flight: that thread may still hold an IRQ cap
        // on the line. The note is sticky, so its own exit_current runs the reclaim.
        if (not cap_teardown_active())
        {
            console_on_driver_death();
        }
        return 0;
    }

    bool cap_teardown_active()
    {
        return g_teardown_depth > 0;
    }

    void cap_teardown(Thread* c)
    {
        // Preconditions differ from every other entry point here: the caller must NOT
        // hold IrqLock, and must have set c->dying first.
        KICKOS_ASSERT(c->dying);
        {
            IrqLock lock;
            g_teardown_depth++;
        }
        int const cap_end = static_cast<int>(c->cap_capacity);
        int i = 0;
        while (i < cap_end)
        {
            IrqLock lock; // released at the bottom of every chunk: that is the point
            for (int n = 0; n < KCAP_TEARDOWN_CHUNK and i < cap_end; n++, i++)
            {
                CapEntry& e = c->handles[i];
                if (e.type == static_cast<uint8_t>(CapType::CAP_EMPTY))
                {
                    continue;
                }
                obj_close_protocol(c, e, /*teardown=*/true);
                CapEntry const detached = e;
                e.gen++;
                e.type = static_cast<uint8_t>(CapType::CAP_EMPTY);
                e.obj = 0;
                e.rights = 0;
                obj_ref_drop(detached, /*teardown=*/true);
            }
        }
        IrqLock lock;
        // TOTALITY. Neither failure is visible downstream until an object pool has silently
        // leaked a slot:
        //   - no entry survived the sweep (a lost chunk boundary), and
        //   - nobody is still parked on this thread (a CAP_REPLY arm that woke a caller
        //     without unlinking it, which also walks a dead queue into the ready list).
        // The first is O(table), hence debug-only; the second is O(1).
#if KICKOS_DEBUG
        for (int k = 0; k < cap_end; k++)
        {
            KICKOS_DEBUG_ASSERT(c->handles[k].type == static_cast<uint8_t>(CapType::CAP_EMPTY));
        }
#endif
        KICKOS_ASSERT(c->reply_waiters.empty());
        g_teardown_depth--;
    }

    // Seat (or re-seat) a thread's reserved stdout slot (index 0) as a SEND-ONLY (CAP_SIGNAL,
    // no WAIT/TRANSFER) copy of console endpoint `target`. CAP_SIGNAL bumps endpoint_refs but
    // NOT recv_holders, so a client does not hold the dead-endpoint gate open. Written
    // DIRECTLY, since cap_install_at rejects index 0; this and cap_install_defaults are the
    // slot's only writers, and its cap-gen is never bumped, so a client's stale handle can
    // never resolve it. Take the new ref BEFORE dropping any prior one, or re-seating the same
    // endpoint transiently frees it. The thread's own cap_teardown drops this ref at exit.
    // Caller holds IrqLock.
    bool cap_seat_stdout(Thread* t, int target)
    {
        // UNGUARDED against a capacity-0 `t`: see the unchecked precondition in cap.h.
        if (not obj_ref_inc(CapType::CAP_ENDPOINT, target, CAP_SIGNAL))
        {
            return false; // at the ceiling: seat nothing, leave any prior seat alone
        }
        CapEntry& e = t->handles[KOS_CAP_STDOUT];
        bool const had_prior = (e.type != static_cast<uint8_t>(CapType::CAP_EMPTY));
        CapEntry const prior = e;
        e.obj = target;
        e.type = static_cast<uint8_t>(CapType::CAP_ENDPOINT);
        e.rights = CAP_SIGNAL;
        if (had_prior)
        {
            obj_ref_drop(prior, /*teardown=*/false);
        }
        return true;
    }

    void cap_install_defaults(Thread* child)
    {
        // Pre-publish: nothing seated (index 0 empty). The selftest/bring-up world that
        // never publishes is untouched, and its apps fall back to kconsole_write.
        if (g_stdout_target < 0)
        {
            return;
        }
        // A ceiling refusal leaves slot 0 empty, which is the state the child already handles
        // pre-publish, so the spawn is NOT failed over it.
        (void)cap_seat_stdout(child, g_stdout_target);
    }

    // Move the kernel's stdout-target ref to `obj_handle` and seat the publisher's own slot 0
    // on it (D3/S3). Caller holds IrqLock. Take BOTH new refs BEFORE dropping any old one, or
    // re-publishing the SAME endpoint transiently frees it and a ceiling refusal is no longer
    // a clean no-op. The kernel's ref carries rights 0 (identity, no WAIT) so it never bumps
    // recv_holders. It goes through obj_ref_inc / endpoint_ref_drop and never raw
    // endpoint_refs[] arithmetic, so the free-at-zero teardown and waiter guard still apply.
    //
    // The publisher's own seat belongs here and not at the syscall: root was created before
    // any publish, so its slot 0 is empty and cap_install_defaults never seated it. Without
    // this its printf would kos_send(0) -> -KOS_EBADF and fall back to the now-dark kernel
    // path.
    bool cap_console_publish(Thread* publisher, int obj_handle)
    {
        if (not obj_ref_inc(CapType::CAP_ENDPOINT, obj_handle, 0))
        {
            return false;
        }
        if (not cap_seat_stdout(publisher, obj_handle))
        {
            obj_ref_undo(CapType::CAP_ENDPOINT, obj_handle, 0);
            return false;
        }
        if (g_stdout_target >= 0)
        {
            endpoint_ref_drop(g_stdout_target, /*teardown=*/false);
        }
        g_stdout_target = obj_handle;
        return true;
    }
}
