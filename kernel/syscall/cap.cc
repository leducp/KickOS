// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Capability-table manager (see cap.h): the per-thread naming+rights layer over the
// global object pools, plus the object-side refcount (kernel().sem_refs) that owns
// destroy-on-last-close. slotpool.h stays generic: refs[] lives here.

#include <kickos/cap.h>
#include <kickos/console_tx.h> // console_note_driver_death
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/kruntime.h>
#include <kickos/sched.h>
#include <kickos/sync.h>

#include <kickos/sys/errno.h>

namespace kickos
{
    namespace
    {
        // "No stdout target published yet". The all-ones index, which SlotPool never seats
        // (slotpool.h), so no live endpoint handle can equal it. Tested by EQUALITY, never by
        // sign: a live handle spends the whole word, and one whose slot generation has reached
        // 32768 has bit 31 set and is NEGATIVE as an int.
        constexpr int KCAP_STDOUT_NONE = -1;

        // Console stdout target: the GLOBAL gen-encoded endpoint handle a userspace
        // console driver serves. The kernel holds ONE ref on it (moved on re-publish);
        // cap_install_defaults seats a send-only copy at index 0 of every child. See
        // docs/design-m3-console-handover-stageii.md (D3/D4/S3).
        constinit InstanceLocal<int> g_stdout_target = {KCAP_STDOUT_NONE};

        int& stdout_target()
        {
            return g_stdout_target.get();
        }

        // Every .bss datum this module owns, in ONE object. The grouping is load-bearing:
        // CapEntry is 8-aligned, so as separate objects the linker drops four bytes of fill
        // in front of the array, and on microbit that takes a whole 32-byte granule of user
        // arena. Inside the struct the two words land in the array's own tail alignment.
        struct CapState
        {
            // One flat array carved into uniform CHUNKS at boot. The free list is threaded
            // THROUGH THE FREE CHUNKS THEMSELVES, so there is no side table (CapChunkList,
            // cap.h).
            CapEntry chunks[kcap_slab_entries()];
            CapChunkList free_chunks;
            // Threads inside cap_teardown right now. A count, not a flag: a dying thread can
            // be switched out mid-sweep and a second thread can then enter and finish its own
            // sweep first. The two routes that do it are enumerated at cap.h's cap_teardown
            // declaration.
            unsigned teardown_depth;
        };
        // Per instance: the slab IS the capability namespace, and a second kernel's
        // cap_slab_init would hand this one's live runs back to the free list.
        constinit InstanceLocal<CapState> g_cap_all;

        CapState& cap_state()
        {
            return g_cap_all.get();
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

#if KICKOS_HAVE_ASPACE
        // Slot index of the frame run a global handle names, via the live object as above.
        int frame_run_index_of(int obj_handle)
        {
            FrameRun* f = kernel().frame_runs.resolve(obj_handle);
            if (f == nullptr)
            {
                return -1;
            }
            return static_cast<int>(f - kernel().frame_runs.at(0));
        }

        // At refs -> 0 the FRAMES go back and then the slot does. Freeing the slot first would
        // lose the base and page count the release needs.
        void frame_run_ref_drop(int obj_handle, bool teardown)
        {
            (void)teardown;
            int const idx = frame_run_index_of(obj_handle);
            if (idx < 0)
            {
                return; // already gone: cannot happen under correct refcounting
            }
            uint8_t& r = kernel().frame_run_refs[idx];
            if (r > 0)
            {
                r--;
            }
            if (r == 0)
            {
                FrameRun* f = kernel().frame_runs.resolve(obj_handle);
                if (f != nullptr and f->pages > 0)
                {
                    frame_pool_free_run(f->base, f->pages, arch_aspace_granule());
                }
                kernel().frame_runs.free(obj_handle);
            }
        }
#endif

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
                // A live server pins a WAIT-bearing cap, so neither recv_holders nor
                // endpoint_refs can reach 0 while the field is set. A slot freed with it set
                // would leave a chain entry pointing into a reused endpoint.
                KICKOS_ASSERT(e == nullptr or e->server == nullptr);
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
#if KICKOS_HAVE_ASPACE
            case CapType::CAP_FRAME:
            {
                frame_run_ref_drop(e.obj, teardown);
                return;
            }
            case CapType::CAP_ASPACE:
            {
                domain_release(domain_resolve(e.obj)); // null-safe; frees at the last hold
                return;
            }
#endif
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
                        endpoint_server_clear(ep);
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
                            // If that endpoint was the published console, no userspace
                            // driver can ever serve it again. This must PRECEDE the EPIPE
                            // loop below, and the surrounding mask does not order the two:
                            // sched::wake admits a switch for a live closer, and
                            // arch_switch swaps INLINE on the sim and on xtensa LX6, so a
                            // woken peer would observe a console still dark. Sound ahead
                            // of the loop on the teardown path too, cap_teardown's
                            // name-keyed pass having already released this thread's IRQ
                            // caps.
                            //
                            // The note is NOT the reclaim decision, and it is STICKY because
                            // this attempt can legitimately refuse. recv_holders counts
                            // WAIT-bearing caps, so on a two-thread driver it reaches 0 when
                            // the SERVICE thread dies, while the registers belong to the IRQ
                            // thread, which parks on a line cap and is not counted here at
                            // all. console_on_driver_death asks the device separately and
                            // defers while any live thread still holds the register window;
                            // only a thread DEATH can free that window, so exit_current is
                            // the one site that retries.
                            if (e.obj == stdout_target())
                            {
                                console_note_driver_death();
                                console_on_driver_death();
                            }
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
#if KICKOS_HAVE_ASPACE
            case CapType::CAP_FRAME:
            case CapType::CAP_ASPACE:
            {
                return 0; // neither parks a waiter, so a close strands nothing
            }
#endif
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
                Thread* caller = cap_reply_caller(e);
                // The unlink DECIDES, and so must precede every other write: a stale reply
                // cap can resolve to a caller parked on a DIFFERENT server, and this arm
                // owns nothing of that thread (not its call_state, not its wait_result, and
                // least of all its `link`, which is that server's list).
                if (caller != nullptr and not reply_donor_unpark(closer, caller))
                {
                    caller = nullptr;
                }
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
#if KICKOS_HAVE_ASPACE
            case CapType::CAP_FRAME:
            {
                int const idx = frame_run_index_of(obj_handle);
                if (idx < 0)
                {
                    return false;
                }
                *refs = &kernel().frame_run_refs[idx];
                return true;
            }
            case CapType::CAP_ASPACE:
            {
                return false; // the domain's own refcount is this kind's; obj_ref_inc takes it
            }
#endif
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
#if KICKOS_HAVE_ASPACE
    bool frame_run_ref(int obj_handle)
    {
        int const idx = frame_run_index_of(obj_handle);
        if (idx < 0)
        {
            return false;
        }
        uint8_t& r = kernel().frame_run_refs[idx];
        if (r == UINT8_MAX)
        {
            return false;
        }
        r++;
        return true;
    }

    void frame_run_release_by_base(arch_phys_addr_t base)
    {
        if (base == 0)
        {
            return;
        }
        for (int i = 0; i < static_cast<int>(kernel().frame_runs.capacity()); i++)
        {
            FrameRun* f = kernel().frame_runs.at(i);
            if (f != nullptr and f->pages > 0 and f->base == base
                and kernel().frame_run_refs[i] > 0)
            {
                frame_run_ref_drop(kernel().frame_runs.handle_for(i), true);
                return;
            }
        }
    }

    uint8_t frame_run_refcount(int obj_handle)
    {
        int const idx = frame_run_index_of(obj_handle);
        if (idx < 0)
        {
            return 0;
        }
        return kernel().frame_run_refs[idx];
    }

    void frame_run_release(int obj_handle)
    {
        frame_run_ref_drop(obj_handle, false);
    }

    int frame_run_create(arch_phys_addr_t base, uint32_t pages)
    {
        int const obj = kernel().frame_runs.alloc();
        if (obj < 0)
        {
            return -1;
        }
        FrameRun* f = kernel().frame_runs.resolve(obj);
        f->base = base;
        f->pages = pages;
        kernel().frame_run_refs[frame_run_index_of(obj)] = 1; // the creator's own

        return obj;
    }
#endif

    bool obj_ref_inc(CapType type, int obj_handle, uint8_t rights)
    {
#if KICKOS_HAVE_ASPACE
        // Taken here rather than through ref_counters, whose false means "no counter" and
        // would silently take none.
        if (type == CapType::CAP_ASPACE)
        {
            Domain* d = domain_resolve(obj_handle);
            if (d == nullptr)
            {
                return true; // stale handle: nothing to hold, and not a refusal
            }
            if (domain_refcount(d) == UINT16_MAX)
            {
                return false;
            }
            domain_ref(d);
            return true;
        }
#endif
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
#if KICKOS_HAVE_ASPACE
        if (type == CapType::CAP_ASPACE)
        {
            domain_release(domain_resolve(obj_handle)); // null-safe
            return;
        }
#endif
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

    CapEntry* cap_lookup(Thread* c, uint32_t cap_handle)
    {
        if (c == nullptr)
        {
            return nullptr;
        }
        uint32_t const idx = cap_handle & KCAP_INDEX_MASK;
        // Bounded by THIS task's run, not by the codec's index field: an encodable index past
        // the run must fail to resolve, never read a neighbouring run off the end of this
        // one. This is also the test that refuses KOS_CAP_AUTHORITY and KCAP_INVALID, whose
        // index field is the one value the capacity rule keeps out of every run.
        if (idx >= thread_cap_capacity(c))
        {
            return nullptr; // capacity 0 (no run) is caught here too: nothing is in range
        }
        CapEntry& e = *cap_slot(c->caps, idx);
        if (e.type == static_cast<uint8_t>(CapType::CAP_EMPTY))
        {
            return nullptr;
        }
        uint32_t const cgen = cap_handle >> KCAP_INDEX_BITS;
        if (static_cast<uint32_t>(e.gen) != cgen)
        {
            return nullptr;
        }
        return &e;
    }

    void* cap_resolve_e(Thread* c, uint32_t cap_handle, CapType want, uint8_t need, int* err)
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
#if KICKOS_HAVE_ASPACE
        else if (want == CapType::CAP_FRAME)
        {
            p = kernel().frame_runs.resolve(e->obj);
        }
        else if (want == CapType::CAP_ASPACE)
        {
            p = domain_resolve(e->obj);
        }
#endif
        if (p != nullptr)
        {
            *err = 0;
        }
        return p;
    }

    void* cap_resolve(Thread* c, uint32_t cap_handle, CapType want, uint8_t need)
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

    int cap_narrow_authority(Thread* c, uint32_t cap_handle, uint8_t mask)
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

    void cap_console_reset()
    {
        stdout_target() = KCAP_STDOUT_NONE;
    }

    void cap_slab_init()
    {
        cap_state().free_chunks.head = nullptr;
        // Push in reverse so the list comes out in address order and a first attach is
        // deterministic across boots.
        for (uint32_t c = KCAP_SLAB_CHUNKS; c > 0; c--)
        {
            cap_state().free_chunks.push(&cap_state().chunks[(c - 1) * KCAP_CHUNK_SLOTS]);
        }
    }

    bool cap_slab_attach(CapRun* run, uint32_t width, uint16_t* free_head, uint16_t* out_width)
    {
        *free_head = KCAP_FREE_NONE;
        *out_width = 0;
        KICKOS_ASSERT(width >= KICKOS_CAP_FIRST_DYNAMIC and width <= KICKOS_MAX_HANDLES);
        uint32_t const chunks = kcap_chunks_for(width);
        if (not cap_state().free_chunks.take(run, chunks))
        {
            return false;
        }
        // CAP_EMPTY is 0 and so is a fresh cap-gen, so a zeroed chunk is an empty table.
        // Required, not tidiness: take() left its own free-list link in entry 0.
        for (uint32_t i = 0; i < chunks; i++)
        {
            kmemset(run->chunk[i], 0, KCAP_CHUNK_SLOTS * sizeof(CapEntry));
        }
        uint32_t capacity = width;
#if KCAP_RUN_CHUNKS == 1
        // One chunk of exactly KICKOS_MAX_HANDLES: a narrower request buys no storage here,
        // so the flat path seats the ceiling and stores nothing.
        capacity = KICKOS_MAX_HANDLES;
#endif
        // The list stops at the capacity, so the chunk-rounded tail stays out of it: an index
        // the tail could hand out is one cap_install would refuse.
        *free_head = cap_run_free_build(*run, capacity);
        *out_width = static_cast<uint16_t>(capacity);
        return true;
    }

    void cap_slab_detach(CapRun* run, uint16_t* free_head, uint16_t* out_width)
    {
        cap_state().free_chunks.give(run);
        // The list lived in the chunks just given back, so a surviving head would name a slot
        // in a chunk the next attach can hand to another task.
        *free_head = KCAP_FREE_NONE;
        *out_width = 0;
    }

    void cap_install_at(Thread* c, int index, int obj_handle, CapType type, uint8_t rights)
    {
        // Index 0 is the kernel stdout slot, written directly by cap_seat_stdout and
        // cap_install_defaults, so this entry point rejects it outright: no delegation or
        // own-create may alias stdout. An out-of-range index is a kernel bug, so it traps in
        // debug and no-ops in release rather than scribble another thread's slot.
        if (index <= KOS_CAP_STDOUT or index >= static_cast<int>(thread_cap_capacity(c)))
        {
            KICKOS_ASSERT(false);
            return;
        }
        CapEntry& e = *cap_slot(c->caps, static_cast<uint32_t>(index));
        // Both halves of the free-list contract: a live entry is not in the list, so
        // overwriting one would strand its reference AND unlink a slot the list never held.
        KICKOS_ASSERT(e.type == static_cast<uint8_t>(CapType::CAP_EMPTY));
        cap_run_free_unlink(c->caps, static_cast<uint32_t>(index), &c->cap_free_head);
        e.obj = obj_handle;
        e.type = static_cast<uint8_t>(type);
        e.rights = rights;
        if (type == CapType::CAP_IRQ)
        {
            c->cap_irq_live++;
        }
    }

    int cap_install(Thread* c, int obj_handle, CapType type, uint8_t rights, uint32_t* out_cap)
    {
        *out_cap = KCAP_INVALID;
        // TAKES NO CHUNK. A full run refuses here, and that refusal is the containment
        // boundary a client's reply mint runs into on a server's table (cap.h).
        uint32_t const index = cap_run_peek_free(c->cap_free_head);
        if (index == KCAP_NO_SLOT)
        {
            return -KOS_EMFILE;
        }
        cap_install_at(c, static_cast<int>(index), obj_handle, type, rights);
        *out_cap = (static_cast<uint32_t>(cap_slot(c->caps, index)->gen) << KCAP_INDEX_BITS)
                   | index;
        return 0;
    }

    int cap_install_reply(Thread* c, Thread* caller, uint32_t* out_cap)
    {
        int const idx = kernel().threads.index_of(caller);
        // Every thread that can issue a syscall holds a slot: idle is the one TCB outside the
        // pool, and kmain creates it with cap_run = CapRun{}, so its capacity is 0 and every
        // cap_lookup fails -KOS_EBADF ahead of any mint or park. Give idle a run and this
        // assert becomes reachable.
        KICKOS_ASSERT(idx >= 0);
        if (cap_reply_live(c) >= KICKOS_CAP_REPLY_MAX)
        {
            *out_cap = KCAP_INVALID;
            return -KOS_EMFILE; // at c's reply bound: the same shape as a full table
        }
        // The handle is stored WHOLE in CapEntry::obj (thread.h asserts the widths match),
        // so this reinterprets a full 32-bit word rather than narrowing it.
        int const rc = cap_install(c, static_cast<int>(kernel().threads.handle_for(idx)),
                                   CapType::CAP_REPLY, 0, out_cap);
        if (rc != 0)
        {
            return rc;
        }
        cap_reply_seq_seat(cap_slot(c->caps, *out_cap & KCAP_INDEX_MASK),
                           static_cast<uint8_t>(caller->call_seq & 0xFF));
#if KCAP_RUN_CHUNKS > 1
        c->cap_reply_live++;
#endif
        return 0;
    }

    uint32_t cap_reply_live(Thread const* c)
    {
#if KCAP_RUN_CHUNKS == 1
        // The flat path is selected by KICKOS_MAX_HANDLES <= KCAP_CHUNK_TARGET, so this scan
        // is at most a granule of entry loads, not the codec's 60000-slot ceiling. An entry
        // is a CAP_REPLY iff cap_install_reply put it there: it is the only mint of that
        // type, and rights 0 makes it undelegable.
        uint32_t n = 0;
        uint32_t const end = thread_cap_capacity(c);
        for (uint32_t i = KICKOS_CAP_FIRST_DYNAMIC; i < end; i++)
        {
            if (cap_slot(c->caps, i)->type == static_cast<uint8_t>(CapType::CAP_REPLY))
            {
                n++;
            }
        }
        return n;
#else
        return c->cap_reply_live;
#endif
    }

    void cap_reply_released(Thread* c)
    {
#if KCAP_RUN_CHUNKS == 1
        (void)c;
#else
        KICKOS_ASSERT(c->cap_reply_live > 0);
        c->cap_reply_live--;
#endif
    }

    bool cap_can_take_reply(Thread* c)
    {
        if (c->cap_free_head == KCAP_FREE_NONE)
        {
            return false;
        }
        return cap_reply_live(c) < KICKOS_CAP_REPLY_MAX;
    }

    Thread* cap_reply_caller(CapEntry const& e)
    {
        uint32_t const u = cap_reply_handle(e);
        uint32_t const index = u & ((1u << ThreadPool::INDEX_BITS) - 1u);
        // The FULL high bits, not truncated to the generation's storage width: a handle
        // carrying anything above the field must fail to resolve rather than alias a live
        // slot.
        uint32_t const gen = u >> ThreadPool::INDEX_BITS;
        uint8_t const seq8 = cap_reply_seq(e);
        ThreadPool& tp = kernel().threads;
        if (index >= static_cast<uint32_t>(tp.next))
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

    namespace
    {
        // The per-type accounting a slot release owes, at the ONE place both release sites
        // reach, so neither can drift from the other.
        void cap_slot_released(Thread* c, CapEntry const& detached)
        {
            if (detached.type == static_cast<uint8_t>(CapType::CAP_REPLY))
            {
                cap_reply_released(c);
            }
            else if (detached.type == static_cast<uint8_t>(CapType::CAP_IRQ))
            {
                KICKOS_DEBUG_ASSERT(c->cap_irq_live > 0);
                c->cap_irq_live--;
            }
        }
    }

    int handle_close(Thread* c, uint32_t cap_handle)
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
        // cleanly reusable and no stale handle resolves during the drop. The release writes
        // the free-list links over `obj`, so it must follow the copy above.
        e->gen++;
        e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
        e->rights = 0;
        cap_run_free_release(c->caps, cap_handle & KCAP_INDEX_MASK, &c->cap_free_head);
        // The CAP_REPLY half of this is the close-instead-of-reply path kos_reply does not cover.
        cap_slot_released(c, detached);
        obj_ref_drop(detached, /*teardown=*/false);
        return 0;
    }

    bool cap_teardown_active()
    {
        return cap_state().teardown_depth > 0;
    }

    namespace
    {
        // Release ONE live entry of a dying thread's table: protocol, stale the handle, empty
        // the slot, then drop the object reference. Both teardown passes go through it, so
        // neither can drift from the other. Caller holds IrqLock and c->dying is set.
        void teardown_entry(Thread* c, uint32_t i)
        {
            CapEntry& e = *cap_slot(c->caps, i);
            obj_close_protocol(c, e, /*teardown=*/true);
            CapEntry const detached = e;
            e.gen++;
            e.type = static_cast<uint8_t>(CapType::CAP_EMPTY);
            e.rights = 0;
            cap_run_free_release(c->caps, i, &c->cap_free_head);
            cap_slot_released(c, detached);
            obj_ref_drop(detached, /*teardown=*/true);
        }
    }

    void cap_teardown(Thread* c)
    {
        // Preconditions differ from every other entry point here: the caller must NOT
        // hold IrqLock, and must have set c->dying first.
        KICKOS_ASSERT(c->dying);
        uint32_t const cap_end = thread_cap_capacity(c);
        {
            IrqLock lock;
            cap_state().teardown_depth++;
            // NAME-KEYED FIRST, and in ONE masked window: an IRQ line is named by NUMBER, so
            // until this thread's binding is detached a peer's irq_claim of the same line
            // answers -KOS_EBUSY. The chunked loop below hands the CPU to peers, including
            // the supervisor its own EPIPE wake releases, so a line swept there is
            // observable as not-yet-released by the very thread that asked for it.
            // Deliberately NOT chunked: a gap inside this pass is a moment when a thread with
            // a counted teardown depth still holds a line, and both console reclaim sites
            // rely on that being impossible. So the masked window may not scale with the
            // table's width: at zero the pass is owed nothing and reads no entry, which is
            // every thread in an image except a driver's IRQ thread.
            if (c->cap_irq_live != 0)
            {
                for (uint32_t k = 0; k < cap_end; k++)
                {
                    if (cap_slot(c->caps, k)->type == static_cast<uint8_t>(CapType::CAP_IRQ))
                    {
                        teardown_entry(c, k);
                    }
                }
            }
#if KICKOS_DEBUG
            // The other direction, and the only one the count cannot catch itself: an install
            // site that forgot to count leaves a line held past the first gap.
            for (uint32_t k = 0; k < cap_end; k++)
            {
                KICKOS_DEBUG_ASSERT(cap_slot(c->caps, k)->type
                                    != static_cast<uint8_t>(CapType::CAP_IRQ));
            }
#endif
        }
        uint32_t i = 0;
        while (i < cap_end)
        {
            IrqLock lock; // released at the bottom of every chunk: that is the point
            for (int n = 0; n < KCAP_TEARDOWN_CHUNK and i < cap_end; n++, i++)
            {
                if (cap_slot(c->caps, i)->type == static_cast<uint8_t>(CapType::CAP_EMPTY))
                {
                    continue;
                }
                teardown_entry(c, i);
            }
        }
        IrqLock lock;
        // TOTALITY. None of these failures is visible downstream until an object pool has
        // silently leaked a slot, or the funnel has read a dangling donor:
        //   - no entry survived the sweep (a lost chunk boundary),
        //   - the reply count agrees with the emptied table (a release arm that forgot
        //     cap_reply_released, which would make the slot's next occupant refuse its first
        //     caller). SEGMENTED BOARDS ONLY: on the flat path cap_reply_live rescans the
        //     table the loop above just emptied, so it restates the check above it,
        //   - the IRQ count agrees with the emptied table (a release arm that forgot to count
        //     one down, which would make the next sweep of this SLOT run a pre-pass it is not
        //     owed; the opposite drift is the debug scan after the pre-pass above),
        //   - nobody is still parked on this thread (a CAP_REPLY arm that woke a caller
        //     without unlinking it, which also walks a dead queue into the ready list), and
        //   - this thread serves no endpoint (an endpoint arm that cleared Endpoint::server
        //     without unlinking the chain, which then outlives the TCB into its next
        //     occupant).
        // The first is O(table), hence debug-only; the rest are O(1) or bounded by a chunk.
#if KICKOS_DEBUG
        for (uint32_t k = 0; k < cap_end; k++)
        {
            KICKOS_DEBUG_ASSERT(cap_slot(c->caps, k)->type
                                == static_cast<uint8_t>(CapType::CAP_EMPTY));
        }
#endif
        KICKOS_ASSERT(cap_reply_live(c) == 0);
        KICKOS_ASSERT(c->cap_irq_live == 0);
        KICKOS_ASSERT(c->reply_waiters.empty());
        KICKOS_ASSERT(c->served_head == EP_SERVED_NONE);
        cap_state().teardown_depth--;
    }

    // Seat (or re-seat) a thread's reserved stdout slot (index 0) as a SEND-ONLY (CAP_SIGNAL,
    // no WAIT/TRANSFER) copy of console endpoint `target`. CAP_SIGNAL bumps endpoint_refs but
    // NOT recv_holders, so a client does not hold the dead-endpoint gate open. Written
    // DIRECTLY, since cap_install_at rejects index 0; this and cap_install_defaults are the
    // only writers that SEAT it. Take the new ref BEFORE dropping any prior one, or re-seating
    // the same endpoint transiently frees it. The thread's own cap_teardown drops this ref at
    // exit. Caller holds IrqLock.
    bool cap_seat_stdout(Thread* t, int target)
    {
        // Slot 0 is written with no bound test below. Ordered before the ref so a runless `t`
        // leaves nothing to undo.
        if (not cap_run_held(t->caps))
        {
            return false;
        }
        if (not obj_ref_inc(CapType::CAP_ENDPOINT, target, CAP_SIGNAL))
        {
            return false; // at the ceiling: seat nothing, leave any prior seat alone
        }
        CapEntry& e = *cap_slot(t->caps, KOS_CAP_STDOUT);
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
        if (stdout_target() == KCAP_STDOUT_NONE)
        {
            return;
        }
        // A ceiling refusal leaves slot 0 empty, which is the state the child already handles
        // pre-publish, so the spawn is NOT failed over it.
        (void)cap_seat_stdout(child, stdout_target());
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
        if (stdout_target() != KCAP_STDOUT_NONE)
        {
            endpoint_ref_drop(stdout_target(), /*teardown=*/false);
        }
        stdout_target() = obj_handle;
        return true;
    }

    bool cap_console_target(int* out)
    {
        if (stdout_target() == KCAP_STDOUT_NONE)
        {
            return false;
        }
        *out = stdout_target();
        return true;
    }
}
