// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Capability-table manager (see cap.h): the per-thread naming+rights layer over the
// global object pools, plus the object-side refcount (kernel().sem_refs) that owns
// destroy-on-last-close. slotpool.h stays generic: refs[] lives here.

#include <kickos/ampwindow.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h> // console_note_driver_death
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/kruntime.h>
#include <kickos/notify.h>
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
        // cap_install_defaults seats a send-only copy at index 0 of every child. The kernel
        // ref closes the publish-to-first-spawn zero-ref window.
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
            return kernel().sems.index_of(kernel().sems.resolve(obj_handle));
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
            return kernel().frame_runs.index_of(kernel().frame_runs.resolve(obj_handle));
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
            return kernel().mutexes.index_of(kernel().mutexes.resolve(obj_handle));
        }

        // Drop one reference to mutex `obj_handle`; free at refs -> 0. Same leak-don't-strand
        // guard as sem_ref_drop: refs -> 0 with a waiter still parked is unreachable via close,
        // because a parked waiter is BLOCKED and cannot run handle_close. refs -> 0 also
        // implies owner == nullptr: an owner's own cap pins a ref through the close-of-owned
        // refusal, and the exit path force-unlocks before this drop.
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
                KICKOS_ASSERT(m == nullptr or m->owner == nullptr); // never free a locked, reachable mutex
                kernel().mutexes.free(obj_handle);
            }
        }

        // Slot index of the endpoint a global handle names (via the live object, as
        // with sems/mutexes). -1 if it does not resolve.
        int endpoint_index_of(int obj_handle)
        {
            return kernel().endpoints.index_of(kernel().endpoints.resolve(obj_handle));
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
            case CapType::CAP_NOTIFY:
            {
                notify_ref_drop(e.obj, teardown);
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
                    return -KOS_EBUSY; // refuse a voluntary close of a mutex you OWN (unlock first)
                }
                // The owner is exiting. Force-unlock BEFORE the ref drop so a
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
                            // Mark console-driver loss before waking EPIPE waiters: inline switching
                            // can run them immediately. Keep the marker if reclaim is deferred while
                            // an IRQ thread still owns the register window. exit_current retries after
                            // thread death releases that window.
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
                return 0; // endpoints NEVER refuse a close, unlike a mutex its owner holds
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
                // NOTHING. A waiter parks on the NOTIFICATION and not on the line, so closing
                // a capability naming this line neither reaches that waiter nor needs to know
                // whether a WAIT-bearing alias survives in this table. The binding leaves its
                // object's signaller chain at irq_ref_drop, once its own last reference goes.
                return 0;
            }
            case CapType::CAP_NOTIFY:
            {
                // NOTHING, and not an oversight: the BIND holds a reference of its own, so
                // the object outlives its last capability, and the bound thread is released
                // by its own kos_notify_unbind or by notify_unbind_self at its death.
                return 0;
            }
            case CapType::CAP_REPLY:
            {
                // m9: run the SAME full stale-resolve as kos_reply before waking. Fires on
                // both voluntary close of a reply cap AND server-death teardown. If the
                // caller is still parked, EPIPE it; the one-shot consume (empty + gen bump)
                // happens at the shared close/teardown site after this returns. A dying
                // closer skips its recompute: it has only the rest of its own sweep left.
#if KICKOS_AMP_NODE
                // A caller in ANOTHER kernel, answered the empty reply here or never: the wire
                // carries no errno, so a service that died and a service that answered nothing
                // both reach that caller the same way. Where the reply ring refuses the
                // publication the obligation is left pending on this node's own bound.
                if (ThreadPool::far_reply_is(static_cast<uint32_t>(cap_reply_handle(e))))
                {
                    amp::inbound_reply(
                        ThreadPool::far_reply_record(static_cast<uint32_t>(cap_reply_handle(e))),
                        nullptr, 0u);
                    if (not teardown)
                    {
                        sched::set_prio(closer, thread_effective_prio(closer));
                    }
                    return 0;
                }
#endif
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
                Endpoint* const ep = kernel().endpoints.resolve(obj_handle);
                int const idx = kernel().endpoints.index_of(ep);
                if (idx < 0)
                {
                    return false;
                }
                *refs = &kernel().endpoint_refs[idx];
                // A cap COPY carrying CAP_WAIT adds a receiver holder.
                if ((rights & CAP_WAIT) != 0)
                {
                    *holders = &ep->recv_holders;
                }
                return true;
            }
            case CapType::CAP_IRQ:
            {
                IrqBinding* const b = kernel().irq_bindings.resolve(obj_handle);
                int const idx = kernel().irq_bindings.index_of(b);
                if (idx < 0)
                {
                    return false;
                }
                *refs = &kernel().irq_refs[idx];
                return true;
            }
            case CapType::CAP_NOTIFY:
            {
                int const idx = kernel().notifies.live_index(obj_handle);
                if (idx < 0)
                {
                    return false;
                }
                *refs = &kernel().notify_refs[idx];
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

    int frame_run_slot_of(int obj_handle)
    {
        return frame_run_index_of(obj_handle);
    }

    void frame_run_release_by_slot(int slot)
    {
        // at() is the bound: a non-null answer is what makes the parallel array below and
        // handle_for() legal on this index.
        FrameRun const* const f = kernel().frame_runs.at(slot);
        if (f == nullptr or f->pages == 0 or kernel().frame_run_refs[slot] == 0)
        {
            return;
        }
        frame_run_ref_drop(kernel().frame_runs.handle_for(slot), true);
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
        // Answers a HANDLE, FRAME_RUN_NONE when there is none: every consumer resolves it, so
        // an index must not leave here.
        int const i = kernel().frame_runs.alloc();
        FrameRun* const f = kernel().frame_runs.at(i); // total over alloc()'s -1
        if (f == nullptr)
        {
            return -1;
        }
        f->base = base;
        f->pages = pages;
        kernel().frame_run_refs[i] = 1; // the creator's own
        return kernel().frame_runs.handle_for(i);
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

    namespace
    {
        // One task's hold sets, a bit per slot of each charged pool.
        struct TaskObjectHolds
        {
            uint32_t sem;
            uint32_t mutex;
            uint32_t endpoint;
            uint32_t irq;
            uint32_t notify;
        };

        // Return the pool hold set and slot for a capability kind.
        // Uncharged kinds return false; invalid handles give slot -1.
        // Use NO_OBJECT when only the hold set is needed.
        constexpr int NO_OBJECT = -1;
        // Keep this inline to avoid an extra stack frame on armv7m
        // when KICKOS_KERNEL_STACKS=0.
        inline __attribute__((always_inline)) bool
        charged_pool(CapType type, int obj_handle, TaskObjectHolds* h, uint32_t** set,
                     int* slot)
        {
            switch (type)
            {
            case CapType::CAP_SEM:
            {
                *set = &h->sem;
                *slot = kernel().sems.live_index(obj_handle);
                return true;
            }
            case CapType::CAP_MUTEX:
            {
                *set = &h->mutex;
                *slot = kernel().mutexes.live_index(obj_handle);
                return true;
            }
            case CapType::CAP_ENDPOINT:
            {
                *set = &h->endpoint;
                *slot = kernel().endpoints.live_index(obj_handle);
                return true;
            }
            case CapType::CAP_IRQ:
            {
                *set = &h->irq;
                *slot = kernel().irq_bindings.live_index(obj_handle);
                return true;
            }
            case CapType::CAP_NOTIFY:
            {
                *set = &h->notify;
                *slot = kernel().notifies.live_index(obj_handle);
                return true;
            }
            default:
            {
                return false;
            }
            }
        }

        // Record the slot `obj_handle` names in its kind's hold set.
        void hold_mark(CapType type, int obj_handle, TaskObjectHolds* h)
        {
            uint32_t* set = nullptr;
            int slot = 0;
            if (not charged_pool(type, obj_handle, h, &set, &slot) or slot < 0)
            {
                return;
            }
            *set = *set | (1u << static_cast<unsigned>(slot));
        }

        // Return the attached notification slot, or -1 for no attachment or an invalid handle.
        // index_of may require division; keep this off hot paths.
        int binding_notify_slot(int binding_handle)
        {
            Kernel& k = kernel();
            IrqBinding const* const b = k.irq_bindings.resolve(binding_handle);
            if (b == nullptr)
            {
                return -1;
            }
            return k.notifies.index_of(b->notify);
        }

        void hold_mark_binding_notify(int binding_handle, TaskObjectHolds* h)
        {
            int const slot = binding_notify_slot(binding_handle);
            if (slot < 0)
            {
                return;
            }
            h->notify = h->notify | (1u << static_cast<unsigned>(slot));
        }

        // Collect the task's objects from its members' capabilities and bindings.
        // Thread and IRQ bindings retain notifications after their capabilities close.
        // Count each pool slot once, even if several references keep it alive.
        void task_object_holds(Task const* t, TaskObjectHolds* out)
        {
            *out = TaskObjectHolds{};
            if (t == nullptr)
            {
                return;
            }
            Kernel& k = kernel();
            for (int i = 0; i < KICKOS_THREAD_SLOTS; i++)
            {
                Thread* const th = &k.threads.slots[i];
                if (th->task != t)
                {
                    continue;
                }
                if (th->notify_bound != KOS_NOTIFY_UNBOUND)
                {
                    hold_mark(CapType::CAP_NOTIFY, notify_bound_handle(th->notify_bound), out);
                }
                uint32_t const end = thread_cap_capacity(th);
                for (uint32_t e = 0; e < end; e++)
                {
                    CapEntry const* const entry = cap_slot(th->caps, e);
                    hold_mark(static_cast<CapType>(entry->type), entry->obj, out);
                    if (entry->type == static_cast<uint8_t>(CapType::CAP_IRQ))
                    {
                        hold_mark_binding_notify(entry->obj, out);
                    }
                }
            }
        }
    }

    bool task_object_admit(CapType kind, Task const* t)
    {
        if (t == nullptr)
        {
            return true;
        }
        TaskObjectHolds holds;
        task_object_holds(t, &holds);
        uint32_t* set = nullptr;
        int slot = 0;
        if (not charged_pool(kind, NO_OBJECT, &holds, &set, &slot))
        {
            return true; // No budget for this kind.
        }
        return task_object_count(*set) < task_object_ceiling(kind);
    }

    namespace
    {
        // Add a slot to the hold set unless it would exceed the budget.
        // Repeated slots count once, regardless of grant order.
        bool stage_hold(CapType kind, int slot, TaskObjectHolds* h)
        {
            uint32_t* set = nullptr;
            int ignored = 0;
            if (not charged_pool(kind, NO_OBJECT, h, &set, &ignored) or slot < 0)
            {
                return true;
            }
            uint32_t const bit = 1u << static_cast<unsigned>(slot);
            if ((*set & bit) != 0)
            {
                return true;
            }
            if (task_object_count(*set) >= task_object_ceiling(kind))
            {
                return false;
            }
            *set = *set | bit;
            return true;
        }
    }

    bool task_object_admit_grants(Task const* t, uint8_t const* packed, int const* objs, int n)
    {
        if (t == nullptr or n <= 0)
        {
            return true; // No task budget or no grants to check.
        }
        TaskObjectHolds holds;
        task_object_holds(t, &holds);
        for (int i = 0; i < n; i++)
        {
            CapType const type = static_cast<CapType>(kcap_grant_type(packed[i]));
            uint32_t* set = nullptr;
            int slot = 0;
            if (not charged_pool(type, objs[i], &holds, &set, &slot))
            {
                continue;
            }
            if (not stage_hold(type, slot, &holds))
            {
                return false;
            }
            // An IRQ grant also adds its attached notification to the destination's holds.
            if (type == CapType::CAP_IRQ
                and not stage_hold(CapType::CAP_NOTIFY, binding_notify_slot(objs[i]), &holds))
            {
                return false;
            }
        }
        return true;
    }

    bool task_object_admit_binding_notify(int binding_slot, int notify_handle)
    {
        Kernel& k = kernel();
        int const notify_slot = k.notifies.live_index(notify_handle);
        if (notify_slot < 0 or binding_slot < 0)
        {
            return true;
        }
        uint32_t const notify_bit = 1u << static_cast<unsigned>(notify_slot);
        uint32_t const irq_bit = 1u << static_cast<unsigned>(binding_slot);
        for (int i = 0; i < KICKOS_MAX_TASKS; i++)
        {
            TaskObjectHolds holds;
            task_object_holds(&k.tasks[i], &holds);
            if ((holds.irq & irq_bit) == 0)
            {
                continue; // This task does not hold the IRQ.
            }
            if ((holds.notify & notify_bit) != 0)
            {
                continue;
            }
            if (task_object_count(holds.notify) >= task_object_ceiling(CapType::CAP_NOTIFY))
            {
                return false;
            }
        }
        return true;
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
        KICKOS_ASSERT_EXCLUSION_HELD();
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
        else if (want == CapType::CAP_NOTIFY)
        {
            p = kernel().notifies.resolve(e->obj);
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

    CapEntry* cap_install_at(Thread* c, int index, int obj_handle, CapType type, uint8_t rights,
                             uint8_t badge)
    {
        // Index 0 is the kernel stdout slot, written directly by cap_seat_stdout and
        // cap_install_defaults, so this entry point rejects it outright: no delegation or
        // own-create may alias stdout. An out-of-range index is a kernel bug, so it traps in
        // debug and no-ops in release rather than scribble another thread's slot.
        if (index <= KOS_CAP_STDOUT or index >= static_cast<int>(thread_cap_capacity(c)))
        {
            KICKOS_ASSERT(false);
            return nullptr;
        }
        CapEntry& e = *cap_slot(c->caps, static_cast<uint32_t>(index));
        // Both halves of the free-list contract: a live entry is not in the list, so
        // overwriting one would strand its reference AND unlink a slot the list never held.
        KICKOS_ASSERT(e.type == static_cast<uint8_t>(CapType::CAP_EMPTY));
        cap_run_free_unlink(c->caps, static_cast<uint32_t>(index), &c->cap_free_head);
        e.obj = obj_handle;
        e.type = static_cast<uint8_t>(type);
        e.rights = rights;
        // A released entry keeps the spare bits its last occupant left, so they are SEATED
        // here and never inherited: an unbadged install has to say so.
        cap_badge_seat(&e, badge);
        if (type == CapType::CAP_IRQ)
        {
            c->cap_irq_live++;
        }
        return &e;
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
        CapEntry const* const e =
            cap_install_at(c, static_cast<int>(index), obj_handle, type, rights,
                           KCAP_BADGE_NONE);
        *out_cap = (static_cast<uint32_t>(e->gen) << KCAP_INDEX_BITS) | index;
        return 0;
    }

    int cap_install_reply(Thread* c, Thread* caller, uint32_t* out_cap)
    {
        *out_cap = KCAP_INVALID;
        int const idx = kernel().threads.index_of(caller);
        // Every thread that can issue a syscall holds a slot: idle is the one TCB outside the
        // pool, and kmain creates it with cap_run = CapRun{}, so its capacity is 0 and every
        // cap_lookup fails -KOS_EBADF ahead of any mint or park. Give idle a run and this
        // assert becomes reachable.
        KICKOS_ASSERT(idx >= 0);
        if (cap_reply_live(c) >= KICKOS_CAP_REPLY_MAX)
        {
            return -KOS_EMFILE; // at c's reply bound: the same shape as a full table
        }
        uint32_t const index = cap_run_peek_free(c->cap_free_head);
        if (index == KCAP_NO_SLOT)
        {
            return -KOS_EMFILE;
        }
        // CapEntry::obj stores the full handle width; thread.h checks the sizes.
        CapEntry* const e =
            cap_install_at(c, static_cast<int>(index),
                           static_cast<int>(kernel().threads.handle_for(idx)),
                           CapType::CAP_REPLY, 0, KCAP_BADGE_NONE);
        cap_reply_seq_seat(e, static_cast<uint8_t>(caller->call_seq & 0xFF));
        *out_cap = (static_cast<uint32_t>(e->gen) << KCAP_INDEX_BITS) | index;
#if KCAP_RUN_CHUNKS > 1
        c->cap_reply_live++;
#endif
        return 0;
    }

    bool cap_uninstall_reply(Thread* c, uint32_t cap, Thread* caller)
    {
        CapEntry* const e = cap_lookup(c, cap);
        if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_REPLY))
        {
            return false;
        }
        // The stored HANDLE and not cap_reply_caller, whose guard also requires the caller to
        // be BLOCKED in CALL_REPLY_WAIT: neither call site has seated that state yet when it
        // retracts, so resolving through it would refuse every legitimate undo. The handle
        // carries the thread's generation, which is the whole of the identity question here.
        int const idx = kernel().threads.index_of(caller);
        if (idx < 0)
        {
            return false;
        }
        if (cap_reply_handle(*e) != static_cast<uint32_t>(kernel().threads.handle_for(idx)))
        {
            return false; // not the reply this caller's mint seated
        }
        // cap_run_free_release writes the free-list links over `obj`, so it must follow the
        // read above. CAP_REPLY holds no object reference, so nothing is dropped.
        e->gen++;
        e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
        e->rights = 0;
        cap_run_free_release(c->caps, cap & KCAP_INDEX_MASK, e, &c->cap_free_head);
        cap_reply_released(c);
        return true;
    }

#if KICKOS_AMP_NODE
    // The same one-shot capability for a caller in ANOTHER kernel, whose handle names a reply
    // RECORD in the pool's reserved band rather than a thread. No sequence is seated: the
    // record holds the far caller's token, and the capability's own generation is what refuses
    // a stale handle.
    int cap_install_far_reply(Thread* c, uint32_t record, uint32_t* out_cap)
    {
        *out_cap = KCAP_INVALID;
        if (cap_reply_live(c) >= KICKOS_CAP_REPLY_MAX)
        {
            return -KOS_EMFILE;
        }
        uint32_t const index = cap_run_peek_free(c->cap_free_head);
        if (index == KCAP_NO_SLOT)
        {
            return -KOS_EMFILE;
        }
        CapEntry const* const e =
            cap_install_at(c, static_cast<int>(index),
                           static_cast<int>(ThreadPool::far_reply_handle(record)),
                           CapType::CAP_REPLY, 0, KCAP_BADGE_NONE);
        *out_cap = (static_cast<uint32_t>(e->gen) << KCAP_INDEX_BITS) | index;
#if KCAP_RUN_CHUNKS > 1
        c->cap_reply_live++;
#endif
        return 0;
    }

    bool cap_uninstall_far_reply(Thread* c, uint32_t cap, uint32_t record)
    {
        CapEntry* const e = cap_lookup(c, cap);
        if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_REPLY))
        {
            return false;
        }
        uint32_t const handle = static_cast<uint32_t>(cap_reply_handle(*e));
        if (not ThreadPool::far_reply_is(handle) or ThreadPool::far_reply_record(handle) != record)
        {
            return false;
        }
        // cap_run_free_release writes the free-list links over `obj`, so it must follow the
        // read above. CAP_REPLY holds no object reference, so nothing is dropped.
        e->gen++;
        e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
        e->rights = 0;
        cap_run_free_release(c->caps, cap & KCAP_INDEX_MASK, e, &c->cap_free_head);
        cap_reply_released(c);
        return true;
    }
#endif

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

    Thread* cap_reply_thread(uint32_t handle, uint32_t seq, uint32_t seq_mask)
    {
        uint32_t const index = handle & ((1u << ThreadPool::INDEX_BITS) - 1u);
        // The FULL high bits, not truncated to the generation's storage width: a handle
        // carrying anything above the field must fail to resolve rather than alias a live
        // slot.
        uint32_t const gen = handle >> ThreadPool::INDEX_BITS;
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
        // The WIDTH IS THE ARM'S, so the narrow local entry and the whole-field far tag run
        // this one clause: an arm comparing more bits than its storage carries would refuse
        // every live call.
        if (((static_cast<uint32_t>(t->call_seq) ^ seq) & seq_mask) != 0u)
        {
            return nullptr; // a newer call rolled the seq (late-reply ABA guard)
        }
        return t;
    }

    Thread* cap_reply_caller(CapEntry const& e)
    {
        return cap_reply_thread(cap_reply_handle(e), cap_reply_seq(e), KCAP_REPLY_SEQ_MASK);
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
        cap_run_free_release(c->caps, cap_handle & KCAP_INDEX_MASK, e, &c->cap_free_head);
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
            cap_run_free_release(c->caps, i, &e, &c->cap_free_head);
            cap_slot_released(c, detached);
            obj_ref_drop(detached, /*teardown=*/true);
        }
    }

    void cap_teardown(Thread* c)
    {
        // Preconditions differ from every other entry point here: the caller must NOT
        // hold IrqLock, and must have set c->dying first.
        KICKOS_ASSERT(c->dying);
#if KICKOS_KERNEL_CORES > 1 && KICKOS_DEBUG
        KICKOS_DEBUG_ASSERT(klock_depth() == 0);
#endif
        uint32_t const cap_end = thread_cap_capacity(c);
        {
            IrqLock lock;
            cap_state().teardown_depth++;
            // Release IRQ lines before the first teardown gap so supervisors can
            // claim them after an EPIPE wake. Keep this pass under one lock; both
            // console reclaim paths rely on it. cap_irq_live skips scanning non-IRQ tables.
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
        // Verify complete teardown: no capabilities, reply/IRQ counts, donors, or
        // served endpoints remain. Full-table checks are debug-only; the others
        // are constant-time or bounded by a capability chunk.
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
        cap_badge_seat(&e, KCAP_BADGE_NONE); // as cap_install_at: never inherited
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

    // Move the kernel console reference and publisher's stdout slot to obj_handle.
    // Caller holds IrqLock. Acquire both new references before releasing old ones
    // so republishing the same endpoint cannot free it or partially fail.
    // The kernel reference has no WAIT right and does not count as a receiver.
    // Install the publisher's slot here because root predates console publication.
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
