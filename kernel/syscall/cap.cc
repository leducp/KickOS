// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Capability-table manager (see cap.h): the per-task naming+rights layer over the
// global object pools, plus the object-side refcount (kernel().sem_refs) that owns
// destroy-on-last-close. slotpool.h stays generic -- refs[] lives here.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>

#include <kickos/sys/errno.h>

namespace kickos
{
    namespace
    {
        // Console stdout target: the GLOBAL gen-encoded endpoint handle a userspace
        // console driver serves, or -1 pre-publish. The kernel holds ONE ref on it
        // (moved on re-publish); cap_install_defaults seats a send-only copy at index 0
        // of every child. See docs/design-m3-console-handover-stageii.md (D3/D4/S3).
        int g_stdout_target = -1;

        // The CapAuthority word of an entry already TYPE-TESTED as CAP_AUTHORITY: that
        // type names no pool object, so `obj` carries the authority instead. Both callers
        // check the type first -- reading it off any other type is a bug.
        uint8_t authority_word(CapEntry const& e)
        {
            return static_cast<uint8_t>(e.obj);
        }

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

        // Drop one reference to semaphore `obj_handle`; free it at refs -> 0. `teardown`
        // = the noreturn exit path: it must never strand a parked waiter, so a would-be
        // free with waiters still linked LEAKS (floors refs at 1). That branch is
        // unreachable via close today (a parked waiter pins its own cap => refs >= 1),
        // so it asserts teardown. Same accounting shape every future pool's arm mirrors.
        void sem_ref_drop(int obj_handle, bool teardown)
        {
            int const idx = sem_index_of(obj_handle);
            if (idx < 0)
            {
                return; // already gone -- cannot happen under correct refcounting
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

        // Drop one reference to mutex `obj_handle`; free at refs -> 0. Same accounting
        // shape as sem_ref_drop, same leak-don't-strand guard: refs -> 0 with a waiter
        // still parked is unreachable via close (a parked waiter is BLOCKED, cannot run
        // handle_close, so its own cap pins refs >= 1), so it asserts teardown and
        // leaks rather than stranding. R4: refs -> 0 also implies owner == nullptr (an
        // owner's own cap pins a ref via the R2 close guard, and R3 force-unlocked
        // before this drop on the exit path) -- assert it.
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

        // Drop one reference to endpoint `obj_handle`; free at refs -> 0. Same accounting
        // shape as sem_ref_drop, but the leak-don't-strand guard checks BOTH waitqs.
        // Unreachable via close: a parked sender pins its own SIGNAL cap and a parked
        // receiver its own WAIT cap (refs >= 1); and recv_holders -> 0 already emptied
        // send_waiters, while recv_waiters requires a WAIT cap (which keeps refs >= 1).
        // So refs -> 0 implies both queues empty; the guard is defense-in-depth.
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

        // Drop one reference to the object a (now-detached) cap entry named; dispatch to
        // the per-type accounting arm. A future type reaching the default without its own
        // arm traps in debug -- a silent skip would leak its reference with no diagnostic
        // (release builds still avoid treating a foreign handle as a sem index: safe leak).
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
            case CapType::CAP_AUTHORITY:
            {
                return; // poolless: the cap IS its rights bits, there is nothing to free
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
        // CAP_SEM has no protocol -- this is why semaphores never needed the hook. The
        // seam #3 (refuse-owned / force-unlock) and #4 (EPIPE-wake) fill their arms here.
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
                // R3: the owner is exiting -- force-unlock BEFORE the ref drop so a
                // waiter is never stranded; the woken lock() caller gets OWNER_DIED.
                mutex_force_unlock(m, closer);
                return 0;
            }
            case CapType::CAP_ENDPOINT:
            {
                // #4: dropping the LAST WAIT-bearing cap makes the endpoint dead -- no
                // receiver can ever exist -- so EPIPE every parked sender. Fired exactly
                // once (recv_holders -> 0), on BOTH voluntary close and exit teardown.
                Endpoint* ep = kernel().endpoints.resolve(e.obj);
                if (ep != nullptr and (e.rights & CAP_WAIT) != 0)
                {
                    // B2: this closer was the conventional server -- drop the dangling
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
                        }
                    }
                }
                return 0; // endpoints NEVER refuse a close (unlike mutex R2)
            }
            case CapType::CAP_AUTHORITY:
            {
                return 0; // no object, no waiters, nothing to refuse a close over
            }
            case CapType::CAP_REPLY:
            {
                // m9: run the SAME full stale-resolve as kos_reply before waking. Fires on
                // both voluntary close of a reply cap AND server-death teardown. If the
                // caller is still parked, EPIPE it; the one-shot consume (empty + gen bump)
                // happens at the shared close/teardown site after this returns. A dying
                // closer skips its recompute (EXITED, never rescheduled).
                Thread* caller = cap_reply_caller(e.obj);
                if (caller != nullptr)
                {
                    caller->call_state = CALL_NONE; // stop the funnel counting this donor
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

    // Public (replaces sem_ref_inc; also the type-agnostic delegation entry point). Bump
    // one reference to the object a global handle names. Handle MUST resolve. Holds IrqLock.
    void obj_ref_inc(CapType type, int obj_handle, uint8_t rights)
    {
        switch (type)
        {
        case CapType::CAP_SEM:
        {
            (void)rights; // sem accounting ignores rights
            int const idx = sem_index_of(obj_handle);
            if (idx < 0)
            {
                return;
            }
            kernel().sem_refs[idx]++;
            return;
        }
        case CapType::CAP_MUTEX:
        {
            (void)rights; // mutex accounting ignores rights
            int const idx = mutex_index_of(obj_handle);
            if (idx < 0)
            {
                return;
            }
            kernel().mutex_refs[idx]++;
            return;
        }
        case CapType::CAP_ENDPOINT:
        {
            int const idx = endpoint_index_of(obj_handle);
            if (idx < 0)
            {
                return;
            }
            kernel().endpoint_refs[idx]++;
            // Delegation COPIES a cap: a WAIT-bearing copy adds a receiver holder.
            if ((rights & CAP_WAIT) != 0)
            {
                kernel().endpoints.at(idx)->recv_holders++;
            }
            return;
        }
        case CapType::CAP_REPLY:
        {
            (void)obj_handle;
            (void)rights;
            return; // names a thread by generational handle: no pool refcount to bump
        }
        case CapType::CAP_AUTHORITY:
        {
            (void)obj_handle;
            (void)rights;
            return; // poolless: no object behind it, so no refcount to bump
        }
        default:
        {
            KICKOS_ASSERT(false); // unknown type must trap in debug
            return;
        }
        }
    }

    CapEntry* cap_lookup(Thread* c, int cap_handle)
    {
        if (c == nullptr)
        {
            return nullptr;
        }
        int const idx = cap_handle & ((1 << KCAP_INDEX_BITS) - 1);
        if (idx >= KICKOS_MAX_HANDLES)
        {
            return nullptr;
        }
        CapEntry& e = c->handles[idx];
        if (e.type == static_cast<uint8_t>(CapType::CAP_EMPTY))
        {
            return nullptr;
        }
        // Full high bits (not truncated): a handle carrying junk above the cap-gen field
        // must fail to resolve, not alias -- mirrors slotpool.h's resolve().
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
        // Read the reserved slot by INDEX, not through cap_lookup: the kernel seats it,
        // so there is no handle to validate and no cap-gen to match. The type test is
        // what makes that safe: a delegated cap landing at index 2 under the i+1
        // packing is not a CAP_AUTHORITY.
        CapEntry const& e = c->handles[KOS_CAP_AUTHORITY];
        if (e.type != static_cast<uint8_t>(CapType::CAP_AUTHORITY))
        {
            return false;
        }
        return (authority_word(e) & need) == need;
    }

    void cap_seat_authority(Thread* t, uint8_t auth)
    {
        CapEntry& e = t->handles[KOS_CAP_AUTHORITY];
        // Mask to AUTH_* bits, so a caller cannot seat a bit no gate reads.
        uint8_t const w = static_cast<uint8_t>(auth & CAP_AUTH_ALL);
        if (w == 0)
        {
            // Clear rather than seat a zero-authority cap: same permission, and an
            // empty slot cannot be mistaken for a held authority.
            e.type = static_cast<uint8_t>(CapType::CAP_EMPTY);
            e.obj = 0;
            e.rights = 0;
            return;
        }
        e.obj = static_cast<int32_t>(w); // the authority word; this type names no object
        e.type = static_cast<uint8_t>(CapType::CAP_AUTHORITY);
        e.rights = 0; // no object right means anything here, and 0 excludes CAP_TRANSFER
    }

    int cap_narrow_authority(Thread* c, int cap_handle, uint8_t mask)
    {
        CapEntry* e = cap_lookup(c, cap_handle);
        if (e == nullptr)
        {
            return -KOS_EBADF;
        }
        if (e->type != static_cast<uint8_t>(CapType::CAP_AUTHORITY))
        {
            // Object caps are out of scope: dropping CAP_WAIT from an endpoint cap has to
            // run the recv_holders accounting obj_close_protocol does, and nothing asks
            // for it yet. The handle argument keeps that generalisation ABI-free.
            return -KOS_EINVAL;
        }
        uint8_t const w = static_cast<uint8_t>(authority_word(*e) & mask);
        if (w == 0)
        {
            // Same clear-not-seat rule as cap_seat_authority, and it is why this goes
            // through the slot rather than writing `obj`: an emptied slot must also lose
            // its type, or cap_check_authority would read a zero word off a live entry.
            e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
            e->obj = 0;
            e->rights = 0;
            return 0;
        }
        e->obj = static_cast<int32_t>(w); // narrowed: & can only clear bits
        return 0;
    }

    void cap_install_at(Thread* c, int index, int obj_handle, CapType type, uint8_t rights)
    {
        // Bounds + reserved-slot guard (defense-in-depth; every caller already validates its
        // index). Index 0 is the kernel stdout slot -- ONLY cap_install_defaults seats it,
        // writing the slot directly, so cap_install_at rejects 0 outright: no delegation or
        // own-create may alias stdout. An out-of-range index is a kernel bug: trap in debug,
        // no-op in release rather than scribble another thread's slot.
        if (index <= KOS_CAP_STDOUT or index >= KICKOS_MAX_HANDLES)
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
        // Own-create placement: scan from KICKOS_CAP_FIRST_DYNAMIC so an own create can NEVER
        // land on a reserved well-known index (0 .. FIRST_DYNAMIC-1). Reserved slots are
        // seated only by the kernel (stdout) or by explicit spawn delegation -- frozen
        // policy, see <kickos/sys/cap_index.h>. Costs FIRST_DYNAMIC slots per table; own
        // caps live in [FIRST_DYNAMIC .. MAX-1].
        for (int i = KICKOS_CAP_FIRST_DYNAMIC; i < KICKOS_MAX_HANDLES; i++)
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
        for (int i = KICKOS_CAP_FIRST_DYNAMIC; i < KICKOS_MAX_HANDLES; i++)
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
            return nullptr; // slot reclaimed under the cap -- stale
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
        return 0;
    }

    void cap_teardown(Thread* c)
    {
        for (int i = 0; i < KICKOS_MAX_HANDLES; i++)
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

    // Seat (or re-seat) a thread's reserved stdout slot (index 0) as a SEND-ONLY
    // (CAP_SIGNAL, no WAIT/TRANSFER) copy of console endpoint `target`. CAP_SIGNAL bumps
    // endpoint_refs but NOT recv_holders (a client is not a receiver), so it does not hold
    // the dead-endpoint gate open. Written DIRECTLY (not via cap_install_at, which rejects
    // index 0): this and cap_install_defaults are the sole writers of the reserved stdout
    // slot, and the slot-0 cap-gen is never bumped (kernel-only policy, so a client's stale
    // handle can never resolve it). Take the new ref BEFORE dropping any prior one (the same
    // take-new-before-drop-old order cap_console_publish uses), so re-seating the same
    // endpoint never transiently frees it. The thread's own cap_teardown drops this ref at
    // exit. Caller holds IrqLock.
    void cap_seat_stdout(Thread* t, int target)
    {
        obj_ref_inc(CapType::CAP_ENDPOINT, target, CAP_SIGNAL);
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
    }

    void cap_install_defaults(Thread* child)
    {
        // Pre-publish: nothing seated (index 0 empty). The selftest/bring-up world that
        // never publishes is untouched, and its apps fall back to kconsole_write.
        if (g_stdout_target < 0)
        {
            return;
        }
        // Post-publish: seat the send-only stdout cap. A fresh child's slot 0 is empty, so
        // cap_seat_stdout takes the ref with no prior to drop.
        cap_seat_stdout(child, g_stdout_target);
    }

    // Move the kernel's stdout-target ref to `obj_handle` (D3/S3). Caller holds IrqLock.
    // Take the new ref BEFORE dropping the old so re-publishing the SAME endpoint never
    // transiently frees it. The ref carries rights 0 (identity, no WAIT) so it never
    // bumps recv_holders. Routed through obj_ref_inc / endpoint_ref_drop, never raw
    // endpoint_refs[] arithmetic, so the free-at-zero teardown + waiter guard still apply.
    void cap_console_publish(int obj_handle)
    {
        obj_ref_inc(CapType::CAP_ENDPOINT, obj_handle, 0);
        if (g_stdout_target >= 0)
        {
            endpoint_ref_drop(g_stdout_target, /*teardown=*/false);
        }
        g_stdout_target = obj_handle;
    }
}
