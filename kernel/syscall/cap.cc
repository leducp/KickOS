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

namespace kickos
{
    namespace
    {
        // Console stdout target: the GLOBAL gen-encoded endpoint handle a userspace
        // console driver serves, or -1 pre-publish. The kernel holds ONE ref on it
        // (moved on re-publish); cap_install_defaults seats a send-only copy at index 0
        // of every child. See docs/design-m3-console-handover-stageii.md (D3/D4/S3).
        int g_stdout_target = -1;

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
                sem_ref_drop(e.obj, teardown);
                return;
            case CapType::CAP_MUTEX:
                mutex_ref_drop(e.obj, teardown);
                return;
            case CapType::CAP_ENDPOINT:
                endpoint_ref_drop(e.obj, teardown);
                return;
            default:
                KICKOS_ASSERT(false);
                return;
            }
        }

        // Per-type close/exit protocol, run BEFORE detach + drop at both call sites.
        // Returns 0, or -1 to refuse a voluntary (non-teardown) close. CAP_SEM has no
        // protocol -- this is why semaphores never needed the hook. The seam #3
        // (refuse-owned / force-unlock) and #4 (EPIPE-wake) fill their arms here.
        int obj_close_protocol(Thread* closer, CapEntry const& e, bool teardown)
        {
            switch (static_cast<CapType>(e.type))
            {
            case CapType::CAP_SEM:
                return 0;
            case CapType::CAP_MUTEX:
            {
                Mutex* m = kernel().mutexes.resolve(e.obj);
                if (m == nullptr or m->owner != closer)
                {
                    return 0; // not the owner: an ordinary refcount close, no protocol
                }
                if (not teardown)
                {
                    return -1; // R2: refuse a voluntary close of a mutex you OWN
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
                if (ep != nullptr and (e.rights & CAP_WAIT) != 0 and ep->recv_holders > 0)
                {
                    ep->recv_holders--;
                    if (ep->recv_holders == 0)
                    {
                        Thread* s;
                        while ((s = wq_pop_highest(ep->send_waiters)) != nullptr)
                        {
                            s->wait_result = -1; // EPIPE
                            sched::wake(s);
                        }
                    }
                }
                return 0; // endpoints NEVER refuse a close (unlike mutex R2)
            }
            default:
                return 0;
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
        default:
            KICKOS_ASSERT(false); // unknown type must trap in debug
            return;
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

    void* cap_resolve(Thread* c, int cap_handle, CapType want, uint8_t need)
    {
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
            return nullptr;
        }
        // WRAP: the stored global handle re-checks the object-gen in its own pool.
        if (want == CapType::CAP_SEM)
        {
            return kernel().sems.resolve(e->obj);
        }
        if (want == CapType::CAP_MUTEX)
        {
            return kernel().mutexes.resolve(e->obj);
        }
        if (want == CapType::CAP_ENDPOINT)
        {
            return kernel().endpoints.resolve(e->obj);
        }
        return nullptr;
    }

    void cap_install_at(Thread* c, int index, int obj_handle, CapType type, uint8_t rights)
    {
        CapEntry& e = c->handles[index];
        e.obj = obj_handle;
        e.type = static_cast<uint8_t>(type);
        e.rights = rights;
    }

    int cap_install(Thread* c, int obj_handle, CapType type, uint8_t rights)
    {
        // Index 0 is the kernel stdout slot (B3): cap_install_defaults seats the published
        // console cap there, and nothing else may. An own create scans from 1 so it never
        // lands at 0. Costs one slot per table (own caps live in [1 .. MAX-1]).
        for (int i = 1; i < KICKOS_MAX_HANDLES; i++)
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

    int handle_close(Thread* c, int cap_handle)
    {
        CapEntry* e = cap_lookup(c, cap_handle);
        if (e == nullptr)
        {
            return -1;
        }
        if (obj_close_protocol(c, *e, /*teardown=*/false) != 0)
        {
            return -1; // protocol refused the close (e.g. #3: owner closing a held mutex)
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

    void cap_install_defaults(Thread* child)
    {
        // Pre-publish: nothing seated (index 0 empty) -- the selftest/bring-up world that
        // never publishes is untouched, and its apps fall back to kconsole_write.
        if (g_stdout_target < 0)
        {
            return;
        }
        // Post-publish: seat a SEND-ONLY (CAP_SIGNAL, no WAIT/TRANSFER) copy of the
        // console endpoint at index 0. CAP_SIGNAL bumps endpoint_refs but NOT recv_holders
        // (a client is not a receiver), so it does not hold the dead-endpoint gate open.
        // The child's own cap_teardown drops this ref at exit.
        cap_install_at(child, 0, g_stdout_target, CapType::CAP_ENDPOINT, CAP_SIGNAL);
        obj_ref_inc(CapType::CAP_ENDPOINT, g_stdout_target, CAP_SIGNAL);
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
