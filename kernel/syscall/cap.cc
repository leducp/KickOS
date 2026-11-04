// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Capability-table manager (see cap.h): the per-thread naming and rights layer over the
// global object pools, plus the object-side refcounts that own destroy-on-last-close.

#include <kickos/ampwindow.h>
#include <kickos/budget.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h> // console_note_driver_death
#include <kickos/image.h>
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
        // Where the whole generation of the object a kind's handle names comes from.
        enum class CapGen : uint8_t
        {
            NONE, // CAP_EMPTY alone
            ENDPOINT_POOL,
            IRQ_POOL,
            NOTIFY_POOL,
            FRAME_POOL,
            DOMAIN,        // the domain slot, whose space an address-space capability names
            THREAD,        // the parked caller's thread slot, or a far caller's reply record
            STATIC_RECORD  // a build-time program's image record, which no slot reuse takes
        };

        // A kind's generation source, and the budget its objects draw on: NO_BUDGET for a kind
        // whose capability names an object no task allocates.
        struct CapKind
        {
            CapType type;
            CapGen gen;
            BudgetKind budget;
        };

        // One row per CapType, in its order.
        constexpr CapKind CAP_KINDS[] = {
            {CapType::CAP_EMPTY, CapGen::NONE, NO_BUDGET},
            {CapType::CAP_ENDPOINT, CapGen::ENDPOINT_POOL, BudgetKind::ENDPOINTS},
            {CapType::CAP_REPLY, CapGen::THREAD, NO_BUDGET},
            {CapType::CAP_IRQ, CapGen::IRQ_POOL, BudgetKind::IRQ_BINDINGS},
            {CapType::CAP_FRAME, CapGen::FRAME_POOL, BudgetKind::FRAME_RUNS},
            {CapType::CAP_ASPACE, CapGen::DOMAIN, NO_BUDGET},
            {CapType::CAP_NOTIFY, CapGen::NOTIFY_POOL, BudgetKind::NOTIFICATIONS},
            {CapType::CAP_PROGRAM, CapGen::STATIC_RECORD, NO_BUDGET},
        };

        constexpr BudgetKind cap_kind_budget(CapType type)
        {
            return CAP_KINDS[static_cast<uint32_t>(type)].budget;
        }

        // The kinds whose budget is a count of the objects a task's members hold.
        constexpr bool held_budget(BudgetKind kind)
        {
            return kind == BudgetKind::ENDPOINTS or kind == BudgetKind::NOTIFICATIONS
                   or kind == BudgetKind::IRQ_BINDINGS;
        }

        constexpr bool cap_kinds_complete()
        {
            constexpr uint32_t rows = sizeof(CAP_KINDS) / sizeof(CAP_KINDS[0]);
            if (rows != static_cast<uint32_t>(CapType::CAP_KIND_MAX))
            {
                return false;
            }
            for (uint32_t i = 0; i < rows; i++)
            {
                if (static_cast<uint32_t>(CAP_KINDS[i].type) != i)
                {
                    return false;
                }
                bool const empty = CAP_KINDS[i].type == CapType::CAP_EMPTY;
                if (empty != (CAP_KINDS[i].gen == CapGen::NONE))
                {
                    return false;
                }
                // Holdings are counted over the three charged pools alone (task_object_holds).
                bool const held = CAP_KINDS[i].type == CapType::CAP_ENDPOINT
                                  or CAP_KINDS[i].type == CapType::CAP_IRQ
                                  or CAP_KINDS[i].type == CapType::CAP_NOTIFY;
                if (held != held_budget(CAP_KINDS[i].budget))
                {
                    return false;
                }
            }
            return true;
        }
        static_assert(cap_kinds_complete(),
                      "every CapType but CAP_EMPTY needs its row in CAP_KINDS, in enum order, "
                      "naming where its object's generation comes from and the budget its "
                      "objects draw on");

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

        // The console endpoint a spawn into `t` seats as its standard output, or KCAP_STDOUT_NONE.
        int stdout_seat_for(Task const* t)
        {
            if (stdout_target() == KCAP_STDOUT_NONE or task_serves_console(t))
            {
                return KCAP_STDOUT_NONE;
            }
            return stdout_target();
        }

        CapEntry* stdout_on_console(Thread const* t)
        {
            if (thread_cap_capacity(t) == 0)
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

        // Every .bss datum this module owns. The slab itself is one flat array in the pools'
        // storage, carved into uniform CHUNKS at boot. The free list is threaded THROUGH THE
        // FREE CHUNKS THEMSELVES, so there is no side table (CapChunkList, cap.h).
        struct CapState
        {
            CapChunkList free_chunks;
            // Threads inside cap_teardown right now. A count, not a flag: a dying thread can
            // be switched out mid-sweep and a second thread can then enter and finish its own
            // sweep first.
            unsigned teardown_depth;
#if KICKOS_KERNEL_CORES > 1
            // The record cap_drop_payer answers inside one locked section of a sweep, and only
            // there: another thread runs in each gap.
            Task const* sweep_payer;
#endif
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
        // FRAME_RUNS or PAGES of the runs `t`'s record pays for: every run any incarnation of
        // it minted, until the run's last holder let it go. O(the record's runs).
        uint32_t run_usage(Task const* t, BudgetKind kind)
        {
            HeadList const* const runs = task_runs_head(t);
            if (runs == nullptr)
            {
                return 0;
            }
            uint32_t used = 0;
            for (ListNode* n = runs->head; n != nullptr; n = n->next)
            {
                if (kind == BudgetKind::FRAME_RUNS)
                {
                    used++;
                }
                else
                {
                    used = used + KICKOS_CONTAINER_OF(n, FrameRun, payer_node)->pages;
                }
            }
            return used;
        }

        // Run slot `idx` leaves its payer's list. O(1).
        void run_unpay(int idx)
        {
            Kernel& k = kernel();
            FrameRun* const f = k.frame_runs.slot(idx);
            if (f->payer == 0)
            {
                return;
            }
            task_runs_head(&k.tasks()[f->payer - 1])->unlink(&f->payer_node);
            f->payer = 0;
        }

        void frame_run_drop(int obj_handle)
        {
            Kernel& k = kernel();
            int const idx = obj_ref_last(k.frame_runs, k.frame_runs.refs(), obj_handle);
            if (idx < 0)
            {
                return;
            }
            // The frames before the slot, which holds their base and page count.
            FrameRun const* const f = k.frame_runs.slot(idx);
            if (f->pages > 0)
            {
                frame_pool_free_run(f->base, f->pages, arch_aspace_granule());
            }
            run_unpay(idx);
            k.frame_runs.free(obj_handle);
        }
#endif

        // Drop one reference to an object and free it at the last. A new type reaching the
        // default without its own arm traps in debug and leaks in release; a silent skip would
        // lose the reference with no diagnostic.
        void obj_ref_drop(CapEntry const& e, bool teardown, Held held)
        {
            Kernel& k = kernel();
            int const obj_handle = cap_obj(e);
            switch (static_cast<CapType>(e.type))
            {
            case CapType::CAP_ENDPOINT:
            {
                // recv_holders -> 0 has already emptied send_waiters, and a receiver parks only
                // with a WAIT cap of its own.
                int const idx = obj_ref_last(k.endpoints, k.endpoints.refs(), obj_handle);
                if (idx < 0)
                {
                    return;
                }
                Endpoint const* const ep = k.endpoints.slot(idx);
                if (leaks(not ep->send_waiters.empty() or not ep->recv_waiters.empty(),
                          &k.endpoints.refs()[idx], teardown))
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
            case CapType::CAP_PROGRAM:
            {
                return; // a static record
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
                return; // holds no reference: the space retires its handles at its task's end
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
            // closer is never rescheduled, so it skips its own recompute.
            if (ep->server == closer)
            {
                endpoint_server_clear(ep);
                // A live closer self-lowers: give up the CPU if a higher thread is now the
                // top runnable.
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

        // Per-type close/exit protocol, run BEFORE detach + drop at both call sites. No kind
        // refuses a close.
        void obj_close_protocol(Thread* closer, CapEntry const& e, bool teardown, Held held)
        {
            switch (static_cast<CapType>(e.type))
            {
            case CapType::CAP_ENDPOINT:
            {
                endpoint_rights_dropped(closer, cap_obj(e), e.rights, teardown, held);
                return;
            }
#if KICKOS_HAVE_ASPACE
            case CapType::CAP_FRAME:
            case CapType::CAP_ASPACE:
            {
                return; // neither parks a waiter, so a close strands nothing
            }
#endif
            case CapType::CAP_PROGRAM:
            {
                return;
            }
            case CapType::CAP_IRQ:
            {
                // NOTHING. A waiter parks on the NOTIFICATION and not on the line, so closing
                // a capability naming this line neither reaches that waiter nor needs to know
                // whether a WAIT-bearing alias survives in this table. The binding leaves its
                // object's signaller chain at irq_ref_drop, once its own last reference goes.
                return;
            }
            case CapType::CAP_NOTIFY:
            {
                // NOTHING, and not an oversight: the BIND holds a reference of its own, so
                // the object outlives its last capability, and the bound thread is released
                // by its own kos_notify_unbind or by notify_unbind_self at its death.
                return;
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
                    // A record that died and seated again since the install is a stranger's.
                    if (cap_obj(e) == e.obj)
                    {
                        amp::inbound_reply(ThreadPool::far_reply_record(cap_reply_handle(e)),
                                           nullptr, 0u);
                    }
                    if (not teardown)
                    {
                        sched::set_prio(closer, thread_effective_prio(closer), held);
                    }
                    return;
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
                return;
            }
            default:
            {
                return;
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
            case CapType::CAP_ENDPOINT:
            {
                int const idx = kernel().endpoints.live_index(obj_handle);
                if (idx < 0)
                {
                    return false;
                }
                Endpoint* const ep = kernel().endpoints.at(idx);
                *refs = &kernel().endpoints.refs()[idx];
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
                *refs = &kernel().irq_bindings.refs()[idx];
                return true;
            }
            case CapType::CAP_NOTIFY:
            {
                int const idx = kernel().notifies.live_index(obj_handle);
                if (idx < 0)
                {
                    return false;
                }
                *refs = &kernel().notifies.refs()[idx];
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
                *refs = &kernel().frame_runs.refs()[idx];
                return true;
            }
            case CapType::CAP_ASPACE:
            {
                return false; // holds no reference
            }
#endif
            case CapType::CAP_REPLY:
            {
                return false; // names a thread by generational handle: no pool refcount
            }
            case CapType::CAP_PROGRAM:
            {
                return false;
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
        uint8_t& r = kernel().frame_runs.refs()[idx];
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
        if (f == nullptr or f->pages == 0 or kernel().frame_runs.refs()[slot] == 0)
        {
            return;
        }
        frame_run_drop(kernel().frame_runs.handle_for(slot));
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

    int frame_run_create_empty(Task const* minter)
    {
        // Answers a HANDLE, FRAME_RUN_NONE when there is none: every consumer resolves it, so
        // an index must not leave here.
        Kernel& k = kernel();
        int const i = k.frame_runs.alloc(minter);
        FrameRun* const f = k.frame_runs.at(i); // total over alloc()'s refusals
        if (f == nullptr)
        {
            return FRAME_RUN_NONE;
        }
        *f = FrameRun{};
        // A run no task mints is the kernel's, which no record pays for.
        HeadList* const runs = task_runs_head(minter);
        if (runs != nullptr)
        {
            f->payer = static_cast<uint8_t>(minter - k.tasks().begin() + 1);
            runs->push(&f->payer_node);
        }
        k.frame_runs.refs()[i] = 1; // the creator's own
        return k.frame_runs.handle_for(i);
    }

    bool frame_run_fill(int obj_handle, arch_phys_addr_t base, uint32_t pages)
    {
        FrameRun* const f = kernel().frame_runs.resolve(obj_handle);
        if (f == nullptr or f->pages != 0 or pages == 0)
        {
            return false;
        }
        f->base = base;
        f->pages = pages;
        return true;
    }

#endif

    bool obj_ref_inc(CapType type, int obj_handle, uint8_t rights)
    {
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

    namespace
    {
        // The pools the holdings count, each the pool of one held kind.
        enum HeldPool : int
        {
            HELD_ENDPOINT,
            HELD_IRQ,
            HELD_NOTIFY,
            HELD_POOLS
        };

        constexpr BudgetKind HELD_KIND[HELD_POOLS] = {
            BudgetKind::ENDPOINTS, BudgetKind::IRQ_BINDINGS, BudgetKind::NOTIFICATIONS};

        // The held pool a capability of `type` names a slot of, -1 for none.
        constexpr int held_of(CapType type)
        {
            if (type == CapType::CAP_ENDPOINT)
            {
                return HELD_ENDPOINT;
            }
            if (type == CapType::CAP_IRQ)
            {
                return HELD_IRQ;
            }
            if (type == CapType::CAP_NOTIFY)
            {
                return HELD_NOTIFY;
            }
            return -1;
        }

        // The held pool whose holdings `kind` counts.
        constexpr int held_of_kind(BudgetKind kind)
        {
            if (kind == BudgetKind::ENDPOINTS)
            {
                return HELD_ENDPOINT;
            }
            if (kind == BudgetKind::IRQ_BINDINGS)
            {
                return HELD_IRQ;
            }
            return HELD_NOTIFY;
        }

        static_assert(HELD_KIND[held_of(CapType::CAP_ENDPOINT)]
                              == cap_kind_budget(CapType::CAP_ENDPOINT)
                          and HELD_KIND[held_of(CapType::CAP_IRQ)]
                                  == cap_kind_budget(CapType::CAP_IRQ)
                          and HELD_KIND[held_of(CapType::CAP_NOTIFY)]
                                  == cap_kind_budget(CapType::CAP_NOTIFY),
                      "each held pool counts the budget its CapType's row in CAP_KINDS names");

        constinit SlotArrays const* const HELD_ARRAYS[HELD_POOLS] = {
            &kickos_kernel_pools.endpoints.arrays, &kickos_kernel_pools.irq_bindings.arrays,
            &kickos_kernel_pools.notifies.arrays};

        // One task's holdings: how many distinct slots of each held pool its members hold, and
        // its budget of each, which is the whole of its ceiling.
        struct TaskObjectHolds
        {
            int count[HELD_POOLS];
            uint32_t budget[HELD_POOLS];
        };

        // The holdings mark of slot `slot` of held pool `k`. A mark is meaningful only for the
        // epoch that set it.
        uint16_t& held_mark(int k, int slot)
        {
            Kernel& kn = kernel();
            if (k == HELD_ENDPOINT)
            {
                return kn.endpoints.slot(slot)->hold_mark;
            }
            if (k == HELD_IRQ)
            {
                return kn.irq_bindings.slot(slot)->hold_mark;
            }
            return kn.notifies.slot(slot)->hold_mark;
        }

        // The live slot of held pool `k` that `handle` names, or -1.
        int held_live_index(int k, int handle)
        {
            Kernel& kn = kernel();
            if (k == HELD_ENDPOINT)
            {
                return kn.endpoints.live_index(handle);
            }
            if (k == HELD_IRQ)
            {
                return kn.irq_bindings.live_index(handle);
            }
            return kn.notifies.live_index(handle);
        }

        // Counts slot `slot` of held pool `k` the first time this epoch reaches it, unless the
        // count is at the budget already: false then, and nothing is counted. A slot of -1 is
        // none, and counts nothing.
        bool stage_hold(int k, int slot, uint16_t epoch, TaskObjectHolds* h)
        {
            if (slot < 0)
            {
                return true;
            }
            uint16_t& mark = held_mark(k, slot);
            if (mark == epoch)
            {
                return true;
            }
            if (static_cast<uint32_t>(h->count[k]) >= h->budget[k])
            {
                return false;
            }
            mark = epoch;
            h->count[k]++;
            return true;
        }

        // A new epoch for one derivation, no mark of which any slot carries yet. The sweep
        // index wraps at the widest held pool, so it reaches each slot of every one within
        // 65534 epochs, fewer than the epoch takes to come round. O(1).
        uint16_t hold_epoch_begin()
        {
            Kernel& k = kernel();
            k.hold_epoch++;
            if (k.hold_epoch == 0)
            {
                k.hold_epoch = 1;
            }
            int const at = k.hold_sweep;
            bool wrap = true;
            for (int p = 0; p < HELD_POOLS; p++)
            {
                int const count = HELD_ARRAYS[p]->count;
                if (at < count)
                {
                    uint16_t& mark = held_mark(p, at);
                    if (mark != k.hold_epoch)
                    {
                        mark = 0;
                    }
                }
                if (at + 1 < count)
                {
                    wrap = false;
                }
            }
            k.hold_sweep = static_cast<uint16_t>(at + 1);
            if (wrap)
            {
                k.hold_sweep = 0;
            }
            return k.hold_epoch;
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

        // Stages the slot a capability of `type` on `obj_handle` names, and for an IRQ binding
        // the notification attached to it. True for a type no held pool counts.
        bool stage_object(CapType type, int obj_handle, uint16_t epoch, TaskObjectHolds* h)
        {
            int const k = held_of(type);
            if (k < 0)
            {
                return true;
            }
            bool staged = stage_hold(k, held_live_index(k, obj_handle), epoch, h);
            if (k == HELD_IRQ)
            {
                staged = stage_hold(HELD_NOTIFY, binding_notify_slot(obj_handle), epoch, h)
                         and staged;
            }
            return staged;
        }

        // `t`'s budget of each held kind, its counts zeroed. Out of task_object_holds, whose
        // frame would otherwise sit above the declaration lookup on every admission chain.
        // Above one kernel core, first ends the charge of each binding whose retirement is over.
        __attribute__((noinline)) void holds_begin(Task const* t, TaskObjectHolds* out)
        {
#if KICKOS_KERNEL_CORES > 1
            irq_retire_drain();
#endif
            for (int k = 0; k < HELD_POOLS; k++)
            {
                out->count[k] = 0;
                out->budget[k] = task_budget(t, HELD_KIND[k]);
            }
        }

        // Count the task's objects from its members' capabilities and bindings, each pool slot
        // once however many references keep it alive: thread and IRQ bindings retain
        // notifications after their capabilities close. A standard output seat naming endpoint
        // `unseated` counts nothing. Marks with `epoch`, and counts each kind up to the budgets
        // holds_begin seated in `out`. O(the capability slots of t's members, plus their
        // bindings).
        void task_object_holds(Task const* t, uint16_t epoch, TaskObjectHolds* out,
                               int unseated = KCAP_STDOUT_NONE)
        {
            if (t == nullptr)
            {
                return;
            }
            for (ListNode* m = task_members(t); m != nullptr; m = m->next)
            {
                Thread* const th = KICKOS_CONTAINER_OF(m, Thread, member);
                if (th->notify_bound != KOS_NOTIFY_UNBOUND)
                {
                    (void)stage_object(CapType::CAP_NOTIFY, notify_bound_handle(th->notify_bound),
                                       epoch, out);
                }
                uint32_t const end = thread_cap_capacity(th);
                for (uint32_t e = 0; e < end; e++)
                {
                    CapEntry const* const entry = cap_slot(th->caps, e);
                    if (e == KOS_CAP_STDOUT and cap_obj(*entry) == unseated
                        and entry->type == static_cast<uint8_t>(CapType::CAP_ENDPOINT))
                    {
                        continue;
                    }
                    (void)stage_object(static_cast<CapType>(entry->type), cap_obj(*entry), epoch,
                                       out);
                }
            }
#if KICKOS_KERNEL_CORES > 1
            // Each binding retiring at its record's charge, and the notification it still
            // raises into until its reclamation.
            Kernel& k = kernel();
            HeadList const* const retiring = task_irq_retiring(t);
            ListNode* n = nullptr;
            if (retiring != nullptr)
            {
                n = retiring->head;
            }
            for (; n != nullptr; n = n->next)
            {
                IrqBinding const* const b = KICKOS_CONTAINER_OF(n, IrqBinding, retiring);
                (void)stage_hold(HELD_IRQ, k.irq_bindings.index_of(b), epoch, out);
                (void)stage_hold(HELD_NOTIFY, k.notifies.index_of(b->notify), epoch, out);
            }
#endif
        }

    }

    namespace
    {
        // Whether `t` can hold endpoint `seat` in place of the standard output seats naming
        // endpoint `was`. O(the capability slots of t's members, plus their bindings).
        bool admit_stdout_swap(Task const* t, int was, int seat)
        {
            if (t == nullptr)
            {
                return true;
            }
            TaskObjectHolds holds;
            holds_begin(t, &holds);
            uint16_t const epoch = hold_epoch_begin();
            task_object_holds(t, epoch, &holds, was);
            return stage_hold(HELD_ENDPOINT, kernel().endpoints.live_index(seat), epoch, &holds);
        }
    }

    uint32_t task_object_usage(Task const* t, BudgetKind kind)
    {
#if KICKOS_HAVE_ASPACE
        if (kind == BudgetKind::FRAME_RUNS or kind == BudgetKind::PAGES)
        {
            return run_usage(t, kind);
        }
#endif
        if (not held_budget(kind) or t == nullptr)
        {
            return 0;
        }
        TaskObjectHolds holds;
        holds_begin(t, &holds);
        task_object_holds(t, hold_epoch_begin(), &holds);
        // Counted up to the budget, which is all an admission needs: a task at its budget
        // takes nothing more.
        return static_cast<uint32_t>(holds.count[held_of_kind(kind)]);
    }

    // O(the capability slots of t's members, plus their bindings, plus n).
    bool task_object_admit_grants(Task const* t, uint8_t const* packed, int const* objs, int n)
    {
        int const seat = stdout_seat_for(t);
        if (t == nullptr or (n <= 0 and seat == KCAP_STDOUT_NONE))
        {
            return true; // No task budget, and nothing to check.
        }
        TaskObjectHolds holds;
        holds_begin(t, &holds);
        uint16_t const epoch = hold_epoch_begin();
        task_object_holds(t, epoch, &holds);
        if (seat != KCAP_STDOUT_NONE
            and not stage_hold(HELD_ENDPOINT, kernel().endpoints.live_index(seat), epoch, &holds))
        {
            return false;
        }
        for (int i = 0; i < n; i++)
        {
            // An IRQ grant also adds its attached notification to the destination's holds.
            if (not stage_object(static_cast<CapType>(kcap_grant_type(packed[i])), objs[i], epoch,
                                 &holds))
            {
                return false;
            }
        }
        return true;
    }

    // O(the capability slots of every task's members, plus their bindings).
    bool task_object_admit_binding_notify(int binding_slot, int notify_handle)
    {
        Kernel& k = kernel();
        int const notify_slot = k.notifies.live_index(notify_handle);
        if (notify_slot < 0 or binding_slot < 0)
        {
            return true;
        }
        for (Task const& t : k.tasks())
        {
            if (task_members(&t) == nullptr)
            {
                continue;
            }
            TaskObjectHolds holds;
            holds_begin(&t, &holds);
            uint16_t const epoch = hold_epoch_begin();
            task_object_holds(&t, epoch, &holds);
            if (held_mark(HELD_IRQ, binding_slot) != epoch)
            {
                continue;
            }
            if (not stage_hold(HELD_NOTIFY, notify_slot, epoch, &holds))
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

    namespace
    {
        // The whole 32-bit generation of the slot `handle` names in a pool of `n`, or 0 past
        // its end.
        uint32_t pool_generation(uint32_t const* gens, uint32_t n, uint32_t handle)
        {
            uint32_t const index = handle_index(handle);
            if (index >= n)
            {
                return 0;
            }
            return gens[index];
        }

        uint32_t thread_generation(uint32_t handle)
        {
#if KICKOS_AMP_NODE
            if (ThreadPool::far_reply_is(handle))
            {
                return amp::inbound_generation(ThreadPool::far_reply_record(handle));
            }
#endif
            ThreadPool const& tp = kernel().threads;
            uint32_t const at = handle & ((1u << ThreadPool::INDEX_BITS) - 1u);
            if (at >= static_cast<uint32_t>(tp.next))
            {
                return 0;
            }
            return tp.wide_gen(static_cast<int>(at));
        }
    }

    uint32_t cap_obj_generation(CapType type, int obj_handle)
    {
        uint32_t const k = static_cast<uint32_t>(type);
        if (k >= static_cast<uint32_t>(CapType::CAP_KIND_MAX))
        {
            return 0;
        }
        uint32_t const raw = static_cast<uint32_t>(obj_handle);
        Kernel& kn = kernel();
        // No default: a source added to CapGen without its arm here fails -Wswitch.
        switch (CAP_KINDS[k].gen)
        {
        case CapGen::NONE:
        case CapGen::STATIC_RECORD:
        {
            return 0;
        }
        case CapGen::ENDPOINT_POOL:
        {
            return pool_generation(kn.endpoints.generations(), kn.endpoints.count(), raw);
        }
        case CapGen::IRQ_POOL:
        {
            return pool_generation(kn.irq_bindings.generations(), kn.irq_bindings.count(), raw);
        }
        case CapGen::NOTIFY_POOL:
        {
            return pool_generation(kn.notifies.generations(), kn.notifies.count(), raw);
        }
        case CapGen::FRAME_POOL:
        {
#if KICKOS_HAVE_ASPACE
            return pool_generation(kn.frame_runs.generations(), kn.frame_runs.count(), raw);
#else
            return 0; // no frame run exists where nothing translates
#endif
        }
        case CapGen::DOMAIN:
        {
#if KICKOS_HAVE_ASPACE
            Domain const* const d = domain_resolve(obj_handle);
            if (d != nullptr)
            {
                return d->generation;
            }
#endif
            return 0;
        }
        case CapGen::THREAD:
        {
            return thread_generation(raw);
        }
        }
        return 0;
    }

    bool cap_obj_live(CapEntry const& e)
    {
        return cap_obj_generation(static_cast<CapType>(e.type), e.obj) == e.obj_gen;
    }

    int cap_obj(CapEntry const& e)
    {
        if (not cap_obj_live(e))
        {
            return -1;
        }
        return e.obj;
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
        // WRAP: the stored global handle re-checks the object-gen in its own pool, and the
        // entry's whole generation refuses a slot reused since the install. A stale object
        // (freed under a still-live cap) stays EBADF (set above).
        if (cap_obj(*e) != e->obj)
        {
            return nullptr;
        }
        void* p = nullptr;
        if (want == CapType::CAP_ENDPOINT)
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
        else if (want == CapType::CAP_PROGRAM)
        {
            p = const_cast<kos_image_header*>(image_program(e->obj));
        }
#if KICKOS_HAVE_ASPACE
        else if (want == CapType::CAP_FRAME)
        {
            // A run still being minted names no frame yet.
            FrameRun* const f = kernel().frame_runs.resolve(e->obj);
            if (f != nullptr and f->pages != 0)
            {
                p = f;
            }
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

    bool cap_check_authority(Thread* c, uint32_t need)
    {
        if (c == nullptr)
        {
            return false; // no caller context: refuse rather than assume the kernel
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
                return -KOS_EBADF; // nothing to give up
            }
            c->authority = c->authority & mask;
            return 0;
        }
        CapEntry* const e = cap_lookup(c, cap_handle);
        if (e == nullptr)
        {
            return -KOS_EBADF;
        }
        if ((cap_handle & KCAP_INDEX_MASK) < KICKOS_CAP_FIRST_DYNAMIC)
        {
            return -KOS_EPERM; // the kernel's seat
        }
        uint8_t const kept = static_cast<uint8_t>(e->rights & mask);
        uint8_t const dropped = static_cast<uint8_t>(e->rights & ~kept);
        e->rights = kept;
        // An endpoint counts its receivers and its handout holders by right, so a right
        // given up here leaves them as a close of a cap carrying it would.
        if (static_cast<CapType>(e->type) == CapType::CAP_ENDPOINT and dropped != 0)
        {
            endpoint_rights_dropped(c, cap_obj(*e), dropped, /*teardown=*/false, held);
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

    CapEntry** cap_directory(int slot)
    {
        return pool_local(kickos_kernel_pools.cap_dirs)
               + static_cast<size_t>(slot) * kickos_kernel_pools.cap_dir_length;
    }

    uint32_t cap_dir_length()
    {
        return kickos_kernel_pools.cap_dir_length;
    }

    uint32_t cap_root_width()
    {
        return kickos_kernel_pools.cap_root_width;
    }

    void cap_slab_init()
    {
        CapEntry* const chunks = pool_local(kickos_kernel_pools.cap_chunks);
        cap_state().free_chunks.head = nullptr;
        // Push in reverse so the list comes out in address order and a first attach is
        // deterministic across boots.
        for (uint32_t c = kickos_kernel_pools.cap_chunk_count; c > 0; c--)
        {
            cap_state().free_chunks.push(&chunks[(c - 1) * KCAP_CHUNK_SLOTS]);
        }
    }

    bool cap_slab_attach(CapEntry** dir, uint32_t width, uint16_t* free_head, uint16_t* out_width)
    {
        *free_head = KCAP_FREE_NONE;
        *out_width = 0;
        uint32_t const length = cap_dir_length();
        uint32_t const chunks = kcap_chunks_for(width);
        KICKOS_ASSERT(width >= KICKOS_CAP_FIRST_DYNAMIC and chunks <= length);
        if (not cap_state().free_chunks.take(dir, chunks, length))
        {
            return false;
        }
        // CAP_EMPTY is 0 and so is a fresh cap-gen, so a zeroed chunk is an empty table.
        // Required, not tidiness: take() left its own free-list link in entry 0.
        for (uint32_t i = 0; i < chunks; i++)
        {
            kmemset(dir[i], 0, KCAP_CHUNK_SLOTS * sizeof(CapEntry));
        }
        // The list stops at the width, so the chunk-rounded tail stays out of it: an index the
        // tail could hand out is one cap_install would refuse.
        uint32_t const capacity = width;
        *free_head = cap_run_free_build(dir, capacity);
        *out_width = static_cast<uint16_t>(capacity);
        return true;
    }

    void cap_slab_detach(CapEntry** dir, uint16_t* free_head, uint16_t* out_width)
    {
        // A TCB no create has seated names no directory, and holds no run.
        if (dir != nullptr)
        {
            cap_state().free_chunks.give(dir, cap_dir_length());
        }
        // The list lived in the chunks just given back, so a surviving head would name a slot
        // in a chunk the next attach can hand to another task.
        *free_head = KCAP_FREE_NONE;
        *out_width = 0;
    }

    CapEntry* cap_install_at(Thread* c, int index, int obj_handle, CapType type, uint8_t rights,
                             uint32_t badge)
    {
        // The reserved indices are the kernel's seats alone, so no delegation or own-create may
        // alias one. An out-of-range index is a kernel bug, so it traps in debug and no-ops in
        // release rather than scribble another thread's slot.
        if (index < KOS_CAP_FIRST_DYNAMIC or index >= static_cast<int>(thread_cap_capacity(c)))
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
        e.obj_gen = cap_obj_generation(type, obj_handle);
        e.type = static_cast<uint8_t>(type);
        e.rights = rights;
        // A released entry keeps the badge and the sequence its last occupant left, so both
        // are SEATED here and never inherited: an unbadged install has to say so.
        cap_badge_seat(&e, badge);
        cap_reply_seq_seat(&e, 0u);
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
        // pool, and kmain creates it with no directory, so its capacity is 0 and every
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
        c->cap_reply_live++;
        return 0;
    }

    void cap_slot_vacate(Thread* c, uint32_t index, CapEntry* e)
    {
        if (e->type == static_cast<uint8_t>(CapType::CAP_REPLY))
        {
            KICKOS_ASSERT(c->cap_reply_live > 0);
            c->cap_reply_live--;
        }
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
        c->cap_reply_live++;
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
        return c->cap_reply_live;
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
        if (static_cast<uint32_t>(tp.gen()[index]) != gen)
        {
            return nullptr; // slot reclaimed under the cap: stale
        }
        Thread* t = &tp.slots()[index];
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
        // The thread slot's whole generation: the handle and the 8-bit sequence both come
        // round on a slot reused often enough.
        if (cap_obj(e) != e.obj)
        {
            return nullptr;
        }
        return cap_reply_thread(cap_reply_handle(e), cap_reply_seq(e), KCAP_REPLY_SEQ_MASK);
    }

    int handle_close(Thread* c, uint32_t cap_handle, Held held)
    {
        CapEntry* e = cap_lookup(c, cap_handle);
        if (e == nullptr)
        {
            return -KOS_EBADF;
        }
        if ((cap_handle & KCAP_INDEX_MASK) < KICKOS_CAP_FIRST_DYNAMIC)
        {
            return -KOS_EPERM; // the kernel's seat
        }
        obj_close_protocol(c, *e, /*teardown=*/false, held);
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

#if KICKOS_KERNEL_CORES > 1
    Task const* cap_drop_payer()
    {
        if (cap_state().sweep_payer != nullptr)
        {
            return cap_state().sweep_payer;
        }
        Thread const* const c = sched::current();
        if (c == nullptr)
        {
            return nullptr;
        }
        return c->task;
    }

    namespace
    {
        // Seats `payer` as cap_drop_payer's answer for one locked section of a sweep, and
        // puts back what it found at the section's end.
        struct SweepPayer
        {
            Task const* outer;

            explicit SweepPayer(Task const* payer) : outer(cap_state().sweep_payer)
            {
                cap_state().sweep_payer = payer;
            }
            ~SweepPayer()
            {
                cap_state().sweep_payer = outer;
            }
        };
    }
#endif

    void cap_teardown(Thread* c, [[maybe_unused]] Task const* payer)
    {
        KICKOS_ASSERT(c->dying);
#if KICKOS_KERNEL_CORES > 1 && KICKOS_DEBUG
        KICKOS_DEBUG_ASSERT(klock_depth() == 0);
#endif
#if KICKOS_KERNEL_CORES > 1
        if (payer == nullptr)
        {
            payer = c->task;
        }
#endif
        uint32_t const cap_end = thread_cap_capacity(c);
        {
            IrqLock lock;
#if KICKOS_KERNEL_CORES > 1
            SweepPayer const paying(payer);
#endif
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
#if KICKOS_KERNEL_CORES > 1
            SweepPayer const paying(payer);
#endif
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
        if (thread_cap_capacity(t) == 0)
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
        e.obj_gen = cap_obj_generation(CapType::CAP_ENDPOINT, target);
        e.type = static_cast<uint8_t>(CapType::CAP_ENDPOINT);
        e.rights = CAP_SIGNAL;
        cap_badge_seat(&e, KCAP_BADGE_NONE); // as cap_install_at: never inherited
        cap_reply_seq_seat(&e, 0u);
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
        int const seat = stdout_seat_for(child->task);
        if (seat == KCAP_STDOUT_NONE)
        {
            return;
        }
        // A ceiling refusal leaves slot 0 empty, which is the state the child already handles
        // pre-publish, so the spawn is NOT failed over it.
        (void)cap_seat_stdout(child, seat, held);
    }

    namespace
    {
        // Moves every standard output seat on the published console to endpoint `seat`, one
        // admission per task: a task its budget refuses keeps the seats it had.
        void stdout_move(int seat, Held held)
        {
            Kernel& k = kernel();
            for (Task& t : k.tasks())
            {
                bool on_console = false;
                for (ListNode* m = task_members(&t); m != nullptr and not on_console; m = m->next)
                {
                    Thread const* const th = KICKOS_CONTAINER_OF(m, Thread, member);
                    on_console = stdout_on_console(th) != nullptr;
                }
                if (not on_console or not admit_stdout_swap(&t, stdout_target(), seat))
                {
                    continue;
                }
                for (ListNode* m = task_members(&t); m != nullptr; m = m->next)
                {
                    Thread* const th = KICKOS_CONTAINER_OF(m, Thread, member);
                    if (stdout_on_console(th) != nullptr)
                    {
                        (void)cap_seat_stdout(th, seat, held);
                    }
                }
            }
            for (int i = 0; i < k.threads.count(); i++)
            {
                Thread* const th = &k.threads.slots()[i];
                if (th->task == nullptr and stdout_on_console(th) != nullptr)
                {
                    (void)cap_seat_stdout(th, seat, held);
                }
            }
        }
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
                stdout_move(obj_handle, held);
                // -KOS_EAGAIN and not the endpoint's own answer: -KOS_ECONNREFUSED makes
                // stdout_write close the seat moved here.
                answer_senders(old, -KOS_EAGAIN, held);
            }
        }
        if (stdout_target() != KCAP_STDOUT_NONE)
        {
            CapEntry stale = {};
            stale.obj = stdout_target();
            stale.obj_gen = cap_obj_generation(CapType::CAP_ENDPOINT, stale.obj);
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
        Endpoint* const ep = kernel().endpoints.resolve(cap_obj(*e));
        if (ep == nullptr)
        {
            return -KOS_EBADF;
        }
        if ((e->rights & CAP_HANDOUT) == 0)
        {
            return -KOS_EACCES;
        }
        if (console_window_held_outside(served_by))
        {
            return -KOS_EBUSY;
        }
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
        for (int i = 0; i < k.threads.count(); i++)
        {
            Thread* const th = &k.threads.slots()[i];
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
