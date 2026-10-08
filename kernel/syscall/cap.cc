// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Capability-table manager (see cap.h): the per-thread naming and rights layer over the
// global object pools, plus the object-side refcounts that own destroy-on-last-close.

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
#include <kickos/task.h>

#include <kickos/sys/errno.h>

namespace kickos
{
    namespace
    {
        // "No stdout target published yet": the all-ones index, which SlotPool never seats.
        // Tested by EQUALITY, never by sign: a live handle may be negative as an int.
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

        CapEntry* stdout_on_console(Thread const* t)
        {
            if (not cap_run_held(t->caps))
            {
                return nullptr;
            }
            CapEntry* const e = cap_slot(t->caps, KOS_CAP_STDOUT);
            if (e->type != static_cast<uint8_t>(CapType::CAP_ENDPOINT) or e->obj != stdout_target())
            {
                return nullptr;
            }
            return e;
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
            // sweep first.
            unsigned teardown_depth;
        };
        // Per instance: the slab IS the capability namespace, and a second kernel's
        // cap_slab_init would hand this one's live runs back to the free list.
        constinit InstanceLocal<CapState> g_cap_all;

        CapState& cap_state()
        {
            return g_cap_all.get();
        }

        // `teardown` is the noreturn exit path, which must never strand a parked waiter: a last
        // reference dropped there with a waiter still linked LEAKS the object instead, refs
        // floored at 1. Unreachable via close, since a parked waiter is BLOCKED and pins its
        // own cap.
        bool leaks(bool parked, uint8_t* refs, bool teardown)
        {
            if (not parked)
            {
                return false;
            }
            KICKOS_ASSERT(teardown);
            *refs = 1;
            return true;
        }

#if KICKOS_HAVE_ASPACE
        // Apart from obj_ref_drop: the address-space release reaches it, and obj_ref_drop's
        // domain arm reaches that release, which would close a call cycle.
        void frame_run_drop(int obj_handle)
        {
            Kernel& k = kernel();
            int const idx = obj_ref_last(k.frame_runs, k.frame_run_refs, obj_handle);
            if (idx < 0)
            {
                return;
            }
            // The frames before the slot, which holds their base and page count.
            FrameRun const* const f = k.frame_runs.at(idx);
            if (f->pages > 0)
            {
                frame_pool_free_run(f->base, f->pages, arch_aspace_granule());
            }
            k.frame_runs.free(obj_handle);
        }
#endif

        // Drop one reference to an object and free it at the last. A new type reaching the
        // default without its own arm traps in debug and leaks in release; a silent skip would
        // lose the reference with no diagnostic.
        void obj_ref_drop(CapEntry const& e, bool teardown, Held held)
        {
            Kernel& k = kernel();
            int const obj_handle = e.obj;
            switch (static_cast<CapType>(e.type))
            {
            case CapType::CAP_SEM:
            {
                int const idx = obj_ref_last(k.sems, k.sem_refs, obj_handle);
                if (idx < 0
                    or leaks(not k.sems.at(idx)->waiters.empty(), &k.sem_refs[idx], teardown))
                {
                    return;
                }
                k.sems.free(obj_handle);
                return;
            }
            case CapType::CAP_MUTEX:
            {
                int const idx = obj_ref_last(k.mutexes, k.mutex_refs, obj_handle);
                if (idx < 0)
                {
                    return;
                }
                Mutex const* const m = k.mutexes.at(idx);
                if (leaks(not m->waiters.empty(), &k.mutex_refs[idx], teardown))
                {
                    return;
                }
                // An owner's own cap pins a ref through the close-of-owned refusal, and the exit
                // path force-unlocks before this drop.
                KICKOS_ASSERT(m->owner == nullptr);
                k.mutexes.free(obj_handle);
                return;
            }
            case CapType::CAP_ENDPOINT:
            {
                // recv_holders -> 0 has already emptied send_waiters, and a receiver parks only
                // with a WAIT cap of its own.
                int const idx = obj_ref_last(k.endpoints, k.endpoint_refs, obj_handle);
                if (idx < 0)
                {
                    return;
                }
                Endpoint const* const ep = k.endpoints.at(idx);
                if (leaks(not ep->send_waiters.empty() or not ep->recv_waiters.empty(),
                          &k.endpoint_refs[idx], teardown))
                {
                    return;
                }
                // A live server pins a WAIT-bearing cap. A slot freed with the field set would
                // leave a chain entry pointing into a reused endpoint.
                KICKOS_ASSERT(ep->server == nullptr);
                k.endpoints.free(obj_handle);
                return;
            }
            case CapType::CAP_REPLY:
            {
                return; // names a thread by generational handle: holds no pool refcount
            }
            case CapType::CAP_IRQ:
            {
                irq_ref_drop(obj_handle, held);
                return;
            }
            case CapType::CAP_NOTIFY:
            {
                notify_ref_drop(obj_handle);
                return;
            }
#if KICKOS_HAVE_ASPACE
            case CapType::CAP_FRAME:
            {
                frame_run_drop(obj_handle);
                return;
            }
            case CapType::CAP_ASPACE:
            {
                domain_release(domain_resolve(obj_handle)); // null-safe; frees at the last hold
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

        // Answer every sender parked on `ep` with `answer`.
        void answer_senders(Endpoint* ep, int32_t answer, Held held)
        {
            Thread* s;
            while ((s = wq_pop_highest(ep->send_waiters)) != nullptr)
            {
                // A SEND_WAIT caller returns via kos_call's call_state clear.
                s->wait_result = answer;
                sched::wake(s, held);
            }
        }

        // Answer every sender parked on `ep` what a new one would be told.
        void refuse_senders(Endpoint* ep, Held held)
        {
            answer_senders(ep, endpoint_unserved(ep, 0), held);
        }

        // `dropped` leaves a cap naming endpoint `obj`, by a close, a teardown or a narrow.
        // Dropping the LAST WAIT-bearing cap leaves the endpoint with no receiver, so every
        // parked sender is answered what a new caller would be: -KOS_EAGAIN while a holder of
        // the handout right remains, -KOS_ECONNREFUSED once none does. Fired exactly once
        // (recv_holders -> 0), on a voluntary close, an exit teardown and a narrow alike. The
        // published console while its task lives is the exception: its senders stay parked.
        void endpoint_rights_dropped(Thread* closer, int obj, uint8_t dropped, bool teardown,
                                     Held held)
        {
            Endpoint* ep = kernel().endpoints.resolve(obj);
            if (ep == nullptr)
            {
                return;
            }
            if ((dropped & CAP_HANDOUT) != 0 and ep->handout_holders > 0)
            {
                ep->handout_holders--;
            }
            if ((dropped & CAP_WAIT) == 0)
            {
                return;
            }
            // This closer was the conventional server: drop the dangling pointer (else a
            // later D2 boost writes a reused TCB) and kill any lingering D2 donation. A dying
            // closer is never rescheduled, so it skips its own recompute (mirrors
            // mutex_force_unlock).
            if (ep->server == closer)
            {
                endpoint_server_clear(ep);
                // A live closer self-lowers: give up the CPU if a higher thread is now the
                // top runnable, mirroring mutex_unlock's no-waiter path.
                if (not teardown)
                {
                    uint8_t const np = thread_effective_prio(closer);
                    if (np != closer->prio)
                    {
                        sched::set_prio(closer, np, held);
                        sched::reschedule(nullptr, held);
                    }
                }
            }
            if (ep->recv_holders == 0)
            {
                return;
            }
            ep->recv_holders--;
            if (ep->recv_holders != 0)
            {
                return;
            }
            ep->vacated = 1;
            if (ep->console == EP_CONSOLE_SERVED)
            {
                return;
            }
            refuse_senders(ep, held);
        }

        // Per-type close/exit protocol, run BEFORE detach + drop at both call sites.
        // Returns 0, or a negative -KOS_E* to refuse a voluntary (non-teardown) close.
        int obj_close_protocol(Thread* closer, CapEntry const& e, bool teardown, Held held)
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
                mutex_force_unlock(m, closer, held);
                return 0;
            }
            case CapType::CAP_ENDPOINT:
            {
                endpoint_rights_dropped(closer, e.obj, e.rights, teardown, held);
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
                // Run the SAME full stale-resolve as kos_reply before waking. Fires on
                // both voluntary close of a reply cap AND server-death teardown. If the
                // caller is still parked, EPIPE it; the one-shot consume (empty + gen bump)
                // happens at the shared close/teardown site after this returns. A dying
                // closer skips its recompute: it has only the rest of its own sweep left.
#if KICKOS_AMP_NODE
                // A caller in ANOTHER kernel, answered the empty reply here or never: the wire
                // carries no errno, so a service that died and a service that answered nothing
                // both reach that caller the same way. Where the reply ring refuses the
                // publication the obligation is left pending on this node's own bound.
                if (ThreadPool::far_reply_is(cap_reply_handle(e)))
                {
                    amp::inbound_reply(
                        ThreadPool::far_reply_record(cap_reply_handle(e)),
                        nullptr, 0u);
                    if (not teardown)
                    {
                        sched::set_prio(closer, thread_effective_prio(closer), held);
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
                // Deflate BEFORE waking: the wake's reschedule must run against our
                // reverted priority, else the woken high-prio caller cannot preempt the
                // still-boosted closer. Mirrors endpoint_reply's deflate-then-wake order.
                if (not teardown)
                {
                    sched::set_prio(closer, thread_effective_prio(closer), held);
                }
                if (caller != nullptr)
                {
                    sched::wake(caller, held);
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
        // plus recv_holders for a WAIT-bearing endpoint cap and handout_holders for one
        // carrying the handout right. False = nothing to move (a poolless type, or a handle
        // that no longer resolves). ONE locator serves both directions, so obj_ref_inc and
        // obj_ref_undo cannot drift apart and cannot move one endpoint counter without the
        // others.
        bool ref_counters(CapType type, int obj_handle, uint8_t rights,
                          uint8_t** refs, uint8_t** holders, uint8_t** handouts)
        {
            *refs = nullptr;
            *holders = nullptr;
            *handouts = nullptr;
            switch (type)
            {
            case CapType::CAP_SEM:
            {
                int const idx = kernel().sems.live_index(obj_handle);
                if (idx < 0)
                {
                    return false;
                }
                *refs = &kernel().sem_refs[idx];
                return true;
            }
            case CapType::CAP_MUTEX:
            {
                int const idx = kernel().mutexes.live_index(obj_handle);
                if (idx < 0)
                {
                    return false;
                }
                *refs = &kernel().mutex_refs[idx];
                return true;
            }
            case CapType::CAP_ENDPOINT:
            {
                int const idx = kernel().endpoints.live_index(obj_handle);
                if (idx < 0)
                {
                    return false;
                }
                Endpoint* const ep = kernel().endpoints.at(idx);
                *refs = &kernel().endpoint_refs[idx];
                // A cap COPY carrying CAP_WAIT adds a receiver holder, and one carrying the
                // handout right a holder that may seat one.
                if ((rights & CAP_WAIT) != 0)
                {
                    *holders = &ep->recv_holders;
                }
                if ((rights & CAP_HANDOUT) != 0)
                {
                    *handouts = &ep->handout_holders;
                }
                return true;
            }
            case CapType::CAP_IRQ:
            {
                int const idx = kernel().irq_bindings.live_index(obj_handle);
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
                int const idx = kernel().frame_runs.live_index(obj_handle);
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

#if KICKOS_HAVE_ASPACE
    bool frame_run_ref(int obj_handle)
    {
        int const idx = kernel().frame_runs.live_index(obj_handle);
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
        return kernel().frame_runs.live_index(obj_handle);
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
        frame_run_drop(kernel().frame_runs.handle_for(slot));
    }

    uint8_t frame_run_refcount(int obj_handle)
    {
        int const idx = kernel().frame_runs.live_index(obj_handle);
        if (idx < 0)
        {
            return 0;
        }
        return kernel().frame_run_refs[idx];
    }

    void frame_run_release(int obj_handle)
    {
        frame_run_drop(obj_handle);
    }

    bool frame_run_sync_owed(int obj_handle)
    {
        FrameRun const* const f = kernel().frame_runs.resolve(obj_handle);
        return f != nullptr and f->sync_owed;
    }

    void frame_run_set_sync_owed(int obj_handle, bool owed)
    {
        FrameRun* const f = kernel().frame_runs.resolve(obj_handle);
        if (f != nullptr)
        {
            f->sync_owed = owed;
        }
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
        f->sync_owed = false;
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
        uint8_t* handouts = nullptr;
        if (not ref_counters(type, obj_handle, rights, &refs, &holders, &handouts))
        {
            return true; // nothing to bump: not a refusal
        }
        // EVERY ceiling is tested before ANY counter moves: an endpoint left with
        // endpoint_refs bumped and a holder count not (or the reverse) would leak a
        // holder into the no-receiver answer that no close could ever take back.
        if (*refs == UINT8_MAX or (holders != nullptr and *holders == UINT8_MAX)
            or (handouts != nullptr and *handouts == UINT8_MAX))
        {
            return false;
        }
        (*refs)++;
        if (holders != nullptr)
        {
            (*holders)++;
        }
        if (handouts != nullptr)
        {
            (*handouts)++;
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
        uint8_t* handouts = nullptr;
        if (not ref_counters(type, obj_handle, rights, &refs, &holders, &handouts))
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
        if (handouts != nullptr and *handouts > 0)
        {
            (*handouts)--;
        }
    }

    // Never defined: only a constant evaluation reaches it, and the call ends that evaluation.
    // Outside the unnamed namespace, so the missing definition draws no warning.
    int a_task_budget_reaches_its_pool();

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

        // A CHARGED POOL IS EITHER ABSENT OR WIDER THAN ITS BUDGET, which is the whole of "no
        // single task can take a pool's last slot": at budget == slots one task empties the pool
        // and the supervisor respawning a driver finds nothing left to respawn it with. The
        // budget floor of 1 makes this also refuse a pool of exactly one slot, where a ceiling
        // could only ever be 0 and the pool would cost .bss nothing could allocate. A ceiling
        // past that is no constant, so the arm stating it fails the build.
        constexpr int ceiling_within(int budget, int pool)
        {
            if (pool != 0 and budget >= pool)
            {
                return a_task_budget_reaches_its_pool();
            }
            return budget;
        }
        template <int>
        struct Constant
        {
        };
        template <int Budget, int Pool>
        concept has_ceiling = requires { typename Constant<ceiling_within(Budget, Pool)>; };
        static_assert(has_ceiling<1, 2> and not has_ceiling<2, 2> and has_ceiling<5, 0>,
                      "ceiling_within no longer refuses exactly a budget reaching its pool");

        template <class T, int N>
        constexpr int pool_width(SlotPool<T, N> Kernel::*)
        {
            return N;
        }

        // A kind charged against Pool: its ceiling is its budget, measured against the width of
        // that same pool.
        template <int Budget, auto Pool>
        struct Charged
        {
            static constexpr int ceiling = ceiling_within(Budget, pool_width(Pool));
            static constexpr auto pool = Pool;
        };

        // Op<arm>::on(<its hold set in h>, args...) for the Charged arm of a kind, `otherwise`
        // for an uncharged kind.
        template <template <class> class Op, class R, class... Args>
        constexpr inline __attribute__((always_inline)) R charged(CapType type, R otherwise,
                                                                  TaskObjectHolds* h,
                                                                  Args... args)
        {
            switch (type)
            {
            case CapType::CAP_SEM:
            {
                return Op<Charged<KICKOS_TASK_SEMAPHORE_BUDGET, &Kernel::sems>>::on(&h->sem,
                                                                                    args...);
            }
            case CapType::CAP_MUTEX:
            {
                return Op<Charged<KICKOS_TASK_MUTEX_BUDGET, &Kernel::mutexes>>::on(&h->mutex,
                                                                                   args...);
            }
            case CapType::CAP_ENDPOINT:
            {
                return Op<Charged<KICKOS_TASK_ENDPOINT_BUDGET, &Kernel::endpoints>>::on(
                    &h->endpoint, args...);
            }
            case CapType::CAP_IRQ:
            {
                return Op<Charged<KICKOS_TASK_IRQ_HANDLE_BUDGET, &Kernel::irq_bindings>>::on(
                    &h->irq, args...);
            }
            case CapType::CAP_NOTIFY:
            {
                return Op<Charged<KICKOS_TASK_NOTIFY_BUDGET, &Kernel::notifies>>::on(&h->notify,
                                                                                     args...);
            }
            default:
            {
                return otherwise;
            }
            }
        }

        // The hold set and the slot of the object named.
        template <class Arm>
        struct Mark
        {
            static inline __attribute__((always_inline)) bool
            on(uint32_t* hold, int obj_handle, uint32_t** set, int* slot)
            {
                *set = hold;
                *slot = (kernel().*Arm::pool).live_index(obj_handle);
                return true;
            }
        };

        template <class Arm>
        struct CeilingOf
        {
            static constexpr int on(uint32_t*)
            {
                return Arm::ceiling;
            }
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
            return charged<Mark>(type, false, h, obj_handle, set, slot);
        }

        // Maximum slots per task of a kind, 0 for an uncharged kind.
        constexpr int task_object_ceiling(CapType kind)
        {
            TaskObjectHolds none;
            return charged<CeilingOf>(kind, 0, &none);
        }

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
                continue;
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
        if ((e->rights & need) != need)
        {
            *err = KOS_EACCES; // named a valid cap but it lacks a required right
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

    bool cap_check_authority(Thread* c, uint32_t need)
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

    void cap_seat_authority(Thread* t, uint32_t auth)
    {
        // Mask to AUTH_* bits, so a caller cannot seat a bit no gate reads.
        t->authority = auth & CAP_AUTH_ALL;
    }

    int cap_narrow(Thread* c, uint32_t cap_handle, uint32_t mask, Held held)
    {
        if (cap_handle == KOS_CAP_AUTHORITY)
        {
            if (c->authority == 0)
            {
                // Nothing to give up. A privileged thread lands here too: its permission
                // does not come from this word, so narrowing it would be a lie.
                return -KOS_EBADF;
            }
            c->authority = c->authority & mask;
            return 0;
        }
        CapEntry* const e = cap_lookup(c, cap_handle);
        if (e == nullptr)
        {
            return -KOS_EBADF;
        }
        uint8_t const kept = static_cast<uint8_t>(e->rights & mask);
        uint8_t const dropped = static_cast<uint8_t>(e->rights & ~kept);
        e->rights = kept;
        // An endpoint counts its receivers and its handout holders by right, so a right
        // given up here leaves them as a close of a cap carrying it would.
        if (static_cast<CapType>(e->type) == CapType::CAP_ENDPOINT and dropped != 0)
        {
            endpoint_rights_dropped(c, e->obj, dropped, /*teardown=*/false, held);
        }
        return 0;
    }

    void cap_console_reset()
    {
        Endpoint* const ep = cap_console_endpoint();
        if (ep != nullptr)
        {
            ep->console = EP_CONSOLE_NONE;
        }
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

    void cap_slot_vacate(Thread* c, uint32_t index, CapEntry* e)
    {
#if KCAP_RUN_CHUNKS > 1
        // The flat path counts live replies by scanning, so it has no count to settle.
        if (e->type == static_cast<uint8_t>(CapType::CAP_REPLY))
        {
            KICKOS_ASSERT(c->cap_reply_live > 0);
            c->cap_reply_live--;
        }
#endif
        if (e->type == static_cast<uint8_t>(CapType::CAP_IRQ))
        {
            KICKOS_DEBUG_ASSERT(c->cap_irq_live > 0);
            c->cap_irq_live--;
        }
        e->gen++;
        e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
        e->rights = 0;
        cap_run_free_release(c->caps, index, e, &c->cap_free_head);
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
        // After the read above. CAP_REPLY holds no object reference, so nothing is dropped.
        cap_slot_vacate(c, cap & KCAP_INDEX_MASK, e);
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
        uint32_t const handle = cap_reply_handle(*e);
        if (not ThreadPool::far_reply_is(handle) or ThreadPool::far_reply_record(handle) != record)
        {
            return false;
        }
        // After the read above. CAP_REPLY holds no object reference, so nothing is dropped.
        cap_slot_vacate(c, cap & KCAP_INDEX_MASK, e);
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

    int handle_close(Thread* c, uint32_t cap_handle, Held held)
    {
        CapEntry* e = cap_lookup(c, cap_handle);
        if (e == nullptr)
        {
            return -KOS_EBADF;
        }
        int const refused = obj_close_protocol(c, *e, /*teardown=*/false, held);
        if (refused != 0)
        {
            return refused; // protocol refused the close (owner closing a held mutex -> -KOS_EBUSY)
        }
        CapEntry const detached = *e;
        // Vacated BEFORE the ref drops, so no stale handle resolves during the drop, and
        // after the copy above, whose obj the vacate overwrites.
        cap_slot_vacate(c, cap_handle & KCAP_INDEX_MASK, e);
        obj_ref_drop(detached, /*teardown=*/false, held);
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
        // neither can drift from the other. `c` is dying.
        void teardown_entry(Thread* c, uint32_t i, Held held)
        {
            CapEntry& e = *cap_slot(c->caps, i);
            obj_close_protocol(c, e, /*teardown=*/true, held);
            CapEntry const detached = e;
            cap_slot_vacate(c, i, &e);
            obj_ref_drop(detached, /*teardown=*/true, held);
        }
    }

    void cap_teardown(Thread* c)
    {
        KICKOS_ASSERT(c->dying);
#if KICKOS_KERNEL_CORES > 1 && KICKOS_DEBUG
        KICKOS_DEBUG_ASSERT(klock_depth() == 0);
#endif
        uint32_t const cap_end = thread_cap_capacity(c);
        {
            IrqLock lock;
            cap_state().teardown_depth++;
#if KICKOS_AMP_NODE
            // Every exit, the slay exit included, reaches here and never the landing.
            if (c->far_hold != 0u)
            {
                amp::hold_release(c->far_hold - 1u);
                c->far_hold = 0u;
            }
#endif
            // Release IRQ lines before the first teardown gap so supervisors can
            // claim them after an EPIPE wake. Keep this pass under one lock; both
            // console reclaim paths rely on it. cap_irq_live skips scanning non-IRQ tables.
            if (c->cap_irq_live != 0)
            {
                for (uint32_t k = 0; k < cap_end; k++)
                {
                    if (cap_slot(c->caps, k)->type == static_cast<uint8_t>(CapType::CAP_IRQ))
                    {
                        teardown_entry(c, k, lock);
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
                teardown_entry(c, i, lock);
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

    // CAP_SIGNAL bumps endpoint_refs but NOT recv_holders, so a client does not hold the
    // dead-endpoint gate open. Written DIRECTLY, since cap_install_at rejects index 0.
    bool cap_seat_stdout(Thread* t, int target, Held held)
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
            obj_ref_drop(prior, /*teardown=*/false, held);
        }
        return true;
    }

    void cap_install_defaults(Thread* child, Held held)
    {
        // Pre-publish: nothing seated (index 0 empty). The selftest/bring-up world that
        // never publishes is untouched, and its apps fall back to kconsole_write. A member of
        // the console's own task would send to itself.
        if (stdout_target() == KCAP_STDOUT_NONE or task_serves_console(child->task))
        {
            return;
        }
        // A ceiling refusal leaves slot 0 empty, which is the state the child already handles
        // pre-publish, so the spawn is NOT failed over it.
        (void)cap_seat_stdout(child, stdout_target(), held);
    }

    // The publisher's slot is seated here because root predates console publication.
    bool cap_console_publish(Thread* publisher, int obj_handle, Held held)
    {
        if (not obj_ref_inc(CapType::CAP_ENDPOINT, obj_handle, 0))
        {
            return false;
        }
        if (not cap_seat_stdout(publisher, obj_handle, held))
        {
            obj_ref_undo(CapType::CAP_ENDPOINT, obj_handle, 0);
            return false;
        }
        Endpoint* const old = cap_console_endpoint();
        if (old != nullptr)
        {
            old->console = EP_CONSOLE_NONE;
            if (stdout_target() != obj_handle)
            {
                // -KOS_EAGAIN and not the endpoint's own answer: -KOS_ECONNREFUSED makes
                // stdout_write close the seat moved here.
                Kernel& k = kernel();
                for (int i = 0; i < KICKOS_THREAD_SLOTS; i++)
                {
                    Thread* const th = &k.threads.slots[i];
                    if (stdout_on_console(th) != nullptr)
                    {
                        (void)cap_seat_stdout(th, obj_handle, held);
                    }
                }
                answer_senders(old, -KOS_EAGAIN, held);
            }
        }
        if (stdout_target() != KCAP_STDOUT_NONE)
        {
            CapEntry stale = {};
            stale.obj = stdout_target();
            stale.type = static_cast<uint8_t>(CapType::CAP_ENDPOINT);
            obj_ref_drop(stale, /*teardown=*/false, held);
        }
        stdout_target() = obj_handle;
        // No task serves it until cap_console_serve names one.
        Endpoint* const ep = cap_console_endpoint();
        if (ep != nullptr)
        {
            ep->console = EP_CONSOLE_ENDED;
        }
        return true;
    }

    int cap_console_publish_through(Thread* publisher, CapEntry* e, Task* served_by, Held held)
    {
        if ((e->rights & CAP_HANDOUT) == 0)
        {
            return -KOS_EACCES;
        }
        if (console_window_held_outside(served_by))
        {
            return -KOS_EBUSY;
        }
        Endpoint* const ep = kernel().endpoints.resolve(e->obj);
        KICKOS_DEBUG_ASSERT(ep != nullptr);
        bool const seat = (e->rights & CAP_WAIT) == 0;
        if (seat and ep->recv_holders == UINT8_MAX)
        {
            return -KOS_EOVERFLOW;
        }
        if (not cap_console_publish(publisher, e->obj, held))
        {
            return -KOS_EOVERFLOW;
        }
        if (seat)
        {
            e->rights = static_cast<uint8_t>(e->rights | CAP_WAIT);
            ep->recv_holders++;
            ep->vacated = 0;
        }
        cap_console_serve(served_by, held);
        return 0;
    }

    void cap_console_serve(Task* t, Held held)
    {
        task_console_serve(t);
        Endpoint* const ep = cap_console_endpoint();
        if (ep == nullptr or t == nullptr)
        {
            return;
        }
        ep->console = EP_CONSOLE_SERVED;
        // A member's stdout naming the console its own task serves would send to itself.
        Kernel& k = kernel();
        for (int i = 0; i < KICKOS_THREAD_SLOTS; i++)
        {
            Thread* const th = &k.threads.slots[i];
            if (th->task != t)
            {
                continue;
            }
            CapEntry* const e = stdout_on_console(th);
            if (e == nullptr)
            {
                continue;
            }
            // No gen bump: KOS_CAP_STDOUT answers again once a later publish seats it.
            CapEntry const prior = *e;
            e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
            e->rights = 0;
            cap_run_free_release(th->caps, KOS_CAP_STDOUT, e, &th->cap_free_head);
            obj_ref_drop(prior, /*teardown=*/false, held);
        }
    }

    void cap_console_task_ended(Held held)
    {
        // Before the senders are woken, who may run at once, and while the slain receivers still
        // wait, so no line is handed to one.
        Endpoint* const ep = cap_console_endpoint();
        if (ep != nullptr)
        {
            ep->console = EP_CONSOLE_ENDED;
        }
        console_note_driver_death();
        console_on_driver_death(held);
        if (ep != nullptr)
        {
            refuse_senders(ep, held);
        }
    }

    Endpoint* cap_console_endpoint()
    {
        if (stdout_target() == KCAP_STDOUT_NONE)
        {
            return nullptr;
        }
        return kernel().endpoints.resolve(stdout_target());
    }

    bool cap_console_serves(Thread const* t)
    {
        if (stdout_target() == KCAP_STDOUT_NONE)
        {
            return false;
        }
        CapEntry const* const e = stdout_on_console(t);
        if (e == nullptr or (e->rights & CAP_SIGNAL) == 0)
        {
            return false;
        }
        Endpoint const* const ep = kernel().endpoints.resolve(e->obj);
        return ep != nullptr and ep->console == EP_CONSOLE_SERVED;
    }
}
