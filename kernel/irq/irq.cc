// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/irq_route.h>
#include <kickos/irq.h>
#include <kickos/instance.h>
#include <kickos/config.h>
#include <kickos/sync.h>
#include <kickos/time.h> // ktime_deadline_arm, for the timed wait
#include <kickos/irqlock.h>
#include <kickos/cap.h>
#include <kickos/sched.h> // sched_admit_mask + sched::set_affinity, for the routed-core pin
#include <kickos/task.h> // task_core_set, the grant the pin is admitted against
#include <kickos/kernel.h> // KICKOS_ASSERT
#include <kickos/debug.h>  // KICKOS_DEBUG_ASSERT
#include <kickos/arch/arch.h>
#include <kickos/bench.h> // the end-to-end span's taking-core stamp
#include <kickos/ktrace.h>

#include <kickos/sys/abi.h>   // KOS_IRQ_LEVEL claim flag
#include <kickos/sys/errno.h>

#if KICKOS_KERNEL_CORES > 1
#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

#include <atomic> // atomic_thread_fence
#endif

namespace kickos
{
    namespace
    {
#if KICKOS_KERNEL_CORES > 1
        // Names no record: the line's own null-object default stands in (kickos/irq.h).
        constexpr uint32_t IRQ_PUB_NONE = 0u;

        // One record per line plus the reserved index 0, so every line can be bound at once.
        constexpr int IRQ_PUB_SLOTS = KICKOS_MAX_IRQ + 1;

        // Set beside the record index while that record retires. Both halves are load-bearing:
        // it fails irq_published's range test, so a dispatch entered on the line runs the
        // null-object default, and it is not IRQ_PUB_NONE, so the line is not claimable until
        // pub_reclaim clears it.
        constexpr uint32_t IRQ_PUB_RETIRING = 0x80000000u;
        static_assert(IRQ_PUB_RETIRING > static_cast<uint32_t>(IRQ_PUB_SLOTS),
                      "a retiring mark inside the record range would dispatch a retired pair");
        // -1 terminates every chain, so an index must survive the link array's narrowing.
        static_assert(IRQ_PUB_SLOTS - 1 <= INT16_MAX,
                      "a record index the link array cannot hold would truncate a chain");

        enum PubState : uint8_t
        {
            PUB_FREE = 0,
            PUB_LIVE = 1,
            // Unpublished, and waiting for the draining batch to clear before it can join one.
            PUB_PENDING = 2,
            // Unpublished, and covered by the sample in g_drain_epoch.
            PUB_DRAINING = 3
        };

        // Immutable from its publication to its reclamation.
        IrqDispatch g_pub[IRQ_PUB_SLOTS];
        uint8_t g_pub_state[IRQ_PUB_SLOTS] = {};
        // The binding slot this record's grace period ALSO gates, or -1.
        int g_pub_binding[IRQ_PUB_SLOTS];
        // The line naming this record, or -1. Reclaiming the record is what frees that line.
        int g_pub_line[IRQ_PUB_SLOTS];
        // The next record on whichever chain this one is on, or -1 for the last. A record is
        // on AT MOST ONE chain, and a LIVE record is on NONE: that is what makes a free pop
        // safe. Slot 0 is on none of them.
        int16_t g_pub_link[IRQ_PUB_SLOTS];
        int16_t g_free_head = -1;
        int16_t g_pending_head = -1;
        int16_t g_draining_head = -1;

        // A53 cache line, so no two cores write one.
        constexpr size_t IRQ_EPOCH_CACHE_LINE = 64u;

        struct alignas(IRQ_EPOCH_CACHE_LINE) EpochRow
        {
            // Odd exactly while that core is inside kickos_isr_irq. Written by the owning core
            // alone, read by every core.
            Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> epoch{0u};
            // Dispatch nesting depth, touched by the owning core alone. The epoch turns over
            // on this field's zero crossings ONLY.
            uint32_t depth = 0;
        };
        static_assert(sizeof(EpochRow) % IRQ_EPOCH_CACHE_LINE == 0,
                      "a row shorter than a line would share one with the next writer");

        EpochRow g_epoch_row[KICKOS_KERNEL_CORES];

        // Each core's epoch as sampled when the draining batch was closed.
        uint32_t g_drain_epoch[KICKOS_KERNEL_CORES] = {};
        bool g_drain_open = false;

        // Store-load ordering, which acquire and release do not give: a core unpublishing a
        // record then sampling an epoch, against a core raising its epoch then reading a
        // publication, must not both read the value from before the other's store.
        void epoch_fence()
        {
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }

        // g_pub_state and the three chains are two records of one membership, kept apart so
        // either can check the other. Compiled out of a shipped image, and called only from
        // the batch paths: it walks the whole record space, which the reserve and the release
        // must not.
        void pub_lists_check()
        {
#if KICKOS_DEBUG
            int16_t const heads[3] = {g_free_head, g_pending_head, g_draining_head};
            uint8_t const want[3] = {PUB_FREE, PUB_PENDING, PUB_DRAINING};
            int chained[PUB_DRAINING + 1] = {};
            for (int c = 0; c < 3; c++)
            {
                int guard = IRQ_PUB_SLOTS;
                for (int i = heads[c]; i >= 0; i = g_pub_link[i])
                {
                    // A chain longer than the record space is a cycle, or a record threaded
                    // onto two of them.
                    KICKOS_DEBUG_ASSERT(guard > 0);
                    guard--;
                    KICKOS_DEBUG_ASSERT(i > 0 and i < IRQ_PUB_SLOTS);
                    KICKOS_DEBUG_ASSERT(g_pub_state[i] == want[c]);
                    chained[want[c]]++;
                }
            }
            int stated[PUB_DRAINING + 1] = {};
            for (int i = 1; i < IRQ_PUB_SLOTS; i++)
            {
                stated[g_pub_state[i]]++;
            }
            KICKOS_DEBUG_ASSERT(chained[PUB_FREE] == stated[PUB_FREE]);
            KICKOS_DEBUG_ASSERT(chained[PUB_PENDING] == stated[PUB_PENDING]);
            KICKOS_DEBUG_ASSERT(chained[PUB_DRAINING] == stated[PUB_DRAINING]);
#endif
        }
#endif

        // Caller holds IrqLock.
        IrqBinding* binding_of_cap(Thread* c, uint32_t cap_handle, uint8_t need, int* err)
        {
            return static_cast<IrqBinding*>(
                cap_resolve_e(c, cap_handle, CapType::CAP_IRQ, need, err));
        }

        // Pin the CAP_WAIT server to the line's routed core before controller access.
        // Controller mask and pending state are shared across the image; cross-core
        // access is unsupported. The claiming thread may delegate to a different server.
        // SIGNAL-only holders do not access controller registers and need no pin.
        // Call before the outer lock: set_affinity may reschedule. Reject a task whose
        // core grant excludes the routed core.
        int pin_to_line_core(Thread* c, uint32_t cap_handle)
        {
#if KICKOS_KERNEL_CORES > 1
            IrqLock lock;
            int err = 0;
            IrqBinding* const b = binding_of_cap(c, cap_handle, CAP_WAIT, &err);
            if (b == nullptr)
            {
                return -err;
            }
            int const dev_core = arch_irq_line_core(b->line);
            if (dev_core < 0)
            {
                return 0;
            }
            uint32_t effective = 0;
            int const arc = sched_admit_mask(1u << static_cast<unsigned>(dev_core),
                                             task_core_set(c->task), MaskBound::SUBSET,
                                             &effective);
            if (arc != 0)
            {
                return arc;
            }
            sched::set_affinity(c, effective);
#else
            (void)c;
            (void)cap_handle;
#endif
            return 0;
        }

        // Caller holds IrqLock.
        // The FIRST arm discards the latch whatever the trigger: a raise latched before the
        // line had an owner would phantom-wake the first wait. After that only LEVEL keeps
        // discarding; clearing for EDGE would drop the coalesced raises this path exists to
        // redeliver, so an EDGE driver with a known-stale latch calls irq_discard.
        void rearm_locked(IrqBinding* b)
        {
            if (not b->needs_rearm)
            {
                return;
            }
            if (b->notify == nullptr)
            {
                // A line that signals nothing must stay masked: unmasking it would open a
                // source whose raise has nowhere to land, which is a LOST interrupt rather
                // than a deferred one. irq_ack refuses ahead of this; the ISR's own null test
                // is what makes that refusal total.
                return;
            }
            b->needs_rearm = false;
            if (not b->armed_once or b->trigger == IRQ_LEVEL)
            {
                irq_line_op(b->line, LineOp::CLEAR);
            }
            b->armed_once = true;
            irq_line_op(b->line, LineOp::UNMASK);
        }

        // ISR context. `arg` is the pre-bound binding, not a line number.
        // Masks the line; the matching unmask is rearm_locked, on the next wait or ack.
        // It reads the binding's OWN copy of the object pointer and the badge, seated at
        // attach: this path may resolve no capability and walk no pool.
        void irq_event_isr(void* arg)
        {
            IrqBinding* b = static_cast<IrqBinding*>(arg);
            KICKOS_BENCH_E2E_ISR_MARK();
            irq_line_op_local(b->line, LineOp::MASK);
            if (b->notify == nullptr)
            {
                // Unreachable: rearm_locked refuses to open a line that signals nothing, so
                // an unattached binding's line is never unmasked. Masked above regardless.
                return;
            }
            (void)notify_raise(b->notify, 1u << b->badge);
        }

        // Null-object default bound to every line with no driver. An unhandled enabled line
        // must be masked or it re-asserts forever. ISR context: mask and count, no I/O.
        // `arg` encodes the line (seeded by irq_init/irq_detach).
        void irq_default_handler(void* arg)
        {
            int line = static_cast<int>(reinterpret_cast<intptr_t>(arg));
            irq_line_op_local(line, LineOp::MASK);
            kernel().irq_spurious_count++;
        }

        void set_default(int irq)
        {
#if KICKOS_KERNEL_CORES > 1
            kernel().irq_table[irq].pub = IRQ_PUB_NONE;
#else
            kernel().irq_table[irq].handler = irq_default_handler;
            kernel().irq_table[irq].arg =
                reinterpret_cast<void*>(static_cast<intptr_t>(irq));
#endif
        }

#if KICKOS_KERNEL_CORES > 1
        // Moves every pending record into the draining batch and samples the epoch each of
        // them must outlive. Only ever called with that batch empty.
        void pub_batch_close()
        {
            KICKOS_DEBUG_ASSERT(g_draining_head < 0);
            if (g_pending_head < 0)
            {
                return;
            }
            g_draining_head = g_pending_head;
            g_pending_head = -1;
            for (int i = g_draining_head; i >= 0; i = g_pub_link[i])
            {
                g_pub_state[i] = PUB_DRAINING;
            }
            epoch_fence();
            for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; core++)
            {
                g_drain_epoch[core] = g_epoch_row[core].epoch.load();
            }
            g_drain_open = true;
            pub_lists_check();
        }

        // Whether every core inside the dispatch entry when the batch closed has left it. An
        // even sample is already out: waiting for it to change would hang the reclamation on a
        // core that takes no further interrupt.
        bool pub_batch_elapsed()
        {
            for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; core++)
            {
                if ((g_drain_epoch[core] & 1u) == 0u)
                {
                    continue;
                }
                if (g_epoch_row[core].epoch.load() == g_drain_epoch[core])
                {
                    return false;
                }
            }
            return true;
        }

        void pub_reclaim(int slot)
        {
            if (g_pub_line[slot] >= 0)
            {
                kernel().irq_table[g_pub_line[slot]].pub = IRQ_PUB_NONE;
            }
            g_pub_line[slot] = -1;
            if (g_pub_binding[slot] >= 0)
            {
                // No dispatch can still hold this slot's address, so the object pointer the
                // ISR read out of it is now unreachable and the reference may go.
                int const bidx = kernel().irq_bindings.live_index(g_pub_binding[slot]);
                if (bidx >= 0)
                {
                    irq_detach_notify(bidx);
                }
                kernel().irq_bindings.free(g_pub_binding[slot]);
            }
            g_pub_binding[slot] = -1;
            g_pub[slot].handler = nullptr;
            g_pub[slot].arg = nullptr;
            g_pub_state[slot] = PUB_FREE;
            g_pub_link[slot] = g_free_head;
            g_free_head = static_cast<int16_t>(slot);
        }

        // Reclaims whatever a past retirement is owed and opens a batch for the rest; a record
        // whose grace period has not elapsed is left to a later call. Two passes: the first
        // clears the batch a past call left open, the second covers what this call retired.
        void pub_drain()
        {
            pub_lists_check();
            for (int pass = 0; pass < 2; pass++)
            {
                if (not g_drain_open)
                {
                    pub_batch_close();
                }
                if (not g_drain_open or not pub_batch_elapsed())
                {
                    return;
                }
                int i = g_draining_head;
                g_draining_head = -1;
                while (i >= 0)
                {
                    // Read before the reclamation, which rethreads this record onto the free
                    // chain and so overwrites its link.
                    int const next = g_pub_link[i];
                    pub_reclaim(i);
                    i = next;
                }
                g_drain_open = false;
            }
        }

        // Takes `line` off its record, masks it, and hands the record to the next batch. The line
        // keeps NAMING that record, marked retiring, until pub_reclaim frees it: a claim of the
        // line is refused until then, so no rebind can arm it under a dispatch that has already
        // read the record. That grace period also gates binding slot `binding_handle`, or nothing
        // when it is -1.
        void line_release(int line, int binding_handle)
        {
            uint32_t const word = kernel().irq_table[line].pub.load();
            uint32_t const slot = word & ~IRQ_PUB_RETIRING;
            if (slot == IRQ_PUB_NONE or slot >= static_cast<uint32_t>(IRQ_PUB_SLOTS))
            {
                kernel().irq_table[line].pub = IRQ_PUB_NONE;
            }
            else
            {
                if (binding_handle >= 0)
                {
                    // Monotone: the record owes this slot until its reclamation, so a second
                    // release of the same line cannot drop it.
                    g_pub_binding[slot] = binding_handle;
                }
                if ((word & IRQ_PUB_RETIRING) == 0u)
                {
                    g_pub_state[slot] = PUB_PENDING;
                    g_pub_link[slot] = g_pending_head;
                    g_pending_head = static_cast<int16_t>(slot);
                    // BEFORE the epoch sample pub_drain takes: a dispatch that still reads a
                    // live record must be one whose core that sample sees inside the entry.
                    kernel().irq_table[line].pub = slot | IRQ_PUB_RETIRING;
                }
            }
            irq_line_op(line, LineOp::MASK);
            pub_drain();
        }

        // Holds a free record for a caller that has not built what it will publish yet, so the
        // publication itself cannot fail; -1 when every record is spoken for. A reserved record
        // is named by no line, so unreserving it is not a reclamation and needs no grace period.
        int pub_reserve()
        {
            int const i = g_free_head;
            if (i < 0)
            {
                return -1;
            }
            g_free_head = g_pub_link[i];
            g_pub_link[i] = -1;
            g_pub_state[i] = PUB_LIVE;
            g_pub_binding[i] = -1;
            g_pub_line[i] = -1;
            return i;
        }

        void pub_unreserve(int slot)
        {
            g_pub_state[slot] = PUB_FREE;
            g_pub_link[slot] = g_free_head;
            g_free_head = static_cast<int16_t>(slot);
        }

        // Seats the pair in a reserved record and names it from `line`; the fields must be seated
        // before the release store that publishes them.
        void pub_commit(int slot, int line, IrqHandler handler, void* arg)
        {
            g_pub[slot].handler = handler;
            g_pub[slot].arg = arg;
            g_pub_line[slot] = line;
            kernel().irq_table[line].pub = static_cast<uint32_t>(slot);
        }

        bool pub_publish(int line, IrqHandler handler, void* arg)
        {
            int const slot = pub_reserve();
            if (slot < 0)
            {
                return false;
            }
            pub_commit(slot, line, handler, arg);
            return true;
        }

        // Every record free and no batch open. Pre-start only.
        void pub_reset()
        {
            for (int i = 0; i < IRQ_PUB_SLOTS; i++)
            {
                g_pub[i].handler = nullptr;
                g_pub[i].arg = nullptr;
                g_pub_state[i] = PUB_FREE;
                g_pub_binding[i] = -1;
                g_pub_line[i] = -1;
                g_pub_link[i] = -1;
            }
            // Lowest index first, and slot 0 stays off every chain.
            g_free_head = -1;
            for (int i = IRQ_PUB_SLOTS - 1; i >= 1; i--)
            {
                g_pub_link[i] = g_free_head;
                g_free_head = static_cast<int16_t>(i);
            }
            g_pending_head = -1;
            g_draining_head = -1;
            g_drain_open = false;
            pub_lists_check();
        }
#endif
    }

#if KICKOS_KERNEL_CORES > 1
    IrqDispatch irq_published(int line)
    {
        uint32_t const slot = kernel().irq_table[line].pub.load();
        if (slot == IRQ_PUB_NONE or slot >= static_cast<uint32_t>(IRQ_PUB_SLOTS))
        {
            IrqDispatch d;
            d.handler = irq_default_handler;
            d.arg = reinterpret_cast<void*>(static_cast<intptr_t>(line));
            return d;
        }
        return g_pub[slot];
    }
#endif

    // Must run before any irq_attach/irq_claim, pre-start: after it no slot is null.
    void irq_init()
    {
        IrqLock lock;
        kernel().irq_spurious_count = 0;
#if KICKOS_KERNEL_CORES > 1
        pub_reset();
#endif
        for (int i = 0; i < KICKOS_MAX_IRQ; i++)
        {
            set_default(i);
        }
    }

    uint32_t irq_spurious_count()
    {
        return kernel().irq_spurious_count;
    }

    bool irq_attach(int irq, IrqHandler handler, void* arg)
    {
        if (irq < 0 or irq >= KICKOS_MAX_IRQ)
        {
            return false;
        }
        // The arch takes this line ahead of kickos_isr_irq, so a binding here would drive
        // nothing and its detach would mask the line the kernel rings on.
        if (arch_irq_line_kernel_owned(irq))
        {
            return false;
        }
        IrqLock lock;
#if KICKOS_KERNEL_CORES > 1
        pub_drain();
        // One driver per line: only a line still naming the null-object default is free.
        if (kernel().irq_table[irq].pub.load() != IRQ_PUB_NONE)
        {
            return false;
        }
        return pub_publish(irq, handler, arg);
#else
        // One driver per line: only a line still holding the null-object default is free.
        if (kernel().irq_table[irq].handler != irq_default_handler)
        {
            return false;
        }
        kernel().irq_table[irq].handler = handler;
        kernel().irq_table[irq].arg = arg;
        return true;
#endif
    }

    void irq_detach(int irq)
    {
        if (irq < 0 or irq >= KICKOS_MAX_IRQ)
        {
            return;
        }
        IrqLock lock;
#if KICKOS_KERNEL_CORES > 1
        line_release(irq, -1); // back to the null-object
#else
        set_default(irq); // the null-object, not a null slot
        irq_line_op(irq, LineOp::MASK);
#endif
    }

    int irq_claim(Thread* c, int line, unsigned int flags, uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
        if (c == nullptr)
        {
            return -KOS_EPERM;
        }
        if (line < 0 or line >= KICKOS_MAX_IRQ)
        {
            return -KOS_EINVAL;
        }
        if ((flags & ~static_cast<unsigned int>(KOS_IRQ_LEVEL)) != 0)
        {
            return -KOS_EINVAL;
        }
        // REFUSED HERE AND NOT AT DETACH: a capability that cannot be closed safely is worse
        // than one that was never handed out. EPERM and not EBUSY, no holder ever freeing it.
        if (arch_irq_line_kernel_owned(line))
        {
            return -KOS_EPERM;
        }
        Kernel& k = kernel();
        // Before the binding pool and before the publication record, as at the three object
        // creators: a task at its ceiling is refused without spending either.
        if (not task_object_admit(CapType::CAP_IRQ, c->task))
        {
            return -KOS_EOVERFLOW; // this task holds its ceiling of bindings already
        }
#if KICKOS_KERNEL_CORES > 1
        // BEFORE THE ALLOCATION BELOW: a retirement may still owe the pool the slot this claim
        // is about to ask for.
        pub_drain();
        // One driver per line: free iff it still names the null-object default. This is also
        // what keeps the console line unclaimable until the kernel's own console_tx_deinit
        // has detached it.
        if (k.irq_table[line].pub.load() != IRQ_PUB_NONE)
        {
            return -KOS_EBUSY;
        }
        // BEFORE the binding and the capability: the publication below must not be able to fail,
        // or a refused claim would have to hand the binding back through the release path, which
        // reads the line to find the record that owes it and would find none.
        int const pub = pub_reserve();
        if (pub < 0)
        {
            return -KOS_ENOMEM;
        }
#else
        // One driver per line: free iff it still holds the null-object default. This is also
        // what keeps the console line unclaimable until the kernel's own console_tx_deinit
        // has detached it.
        if (k.irq_table[line].handler != irq_default_handler)
        {
            return -KOS_EBUSY;
        }
#endif
        int const i = k.irq_bindings.alloc();
        IrqBinding* b = k.irq_bindings.at(i); // total over alloc()'s -1: one refusal, below
        if (b == nullptr)
        {
#if KICKOS_KERNEL_CORES > 1
            pub_unreserve(pub);
#endif
            return -KOS_ENOMEM;
        }
        b->notify = nullptr;
        b->next_signaller = NOTIFY_SIGNALLER_NONE;
        b->badge = 0;
        b->line = line;
        // The first irq_wait arms the line: a claim leaves it masked, so there is no window
        // in which the line is armed and unowned.
        b->needs_rearm = true;
        b->armed_once = false;
        b->trigger = IRQ_EDGE;
        if ((flags & static_cast<unsigned int>(KOS_IRQ_LEVEL)) != 0)
        {
            b->trigger = IRQ_LEVEL;
        }
        k.irq_refs[i] = 1; // the claimer's cap is the first reference
        int const obj = k.irq_bindings.handle_for(i);
        // Install BEFORE attaching: a failure path must never leave a line bound to a slot
        // that is about to be freed.
        uint32_t cap = KCAP_INVALID;
        int const rc = cap_install(c, obj, CapType::CAP_IRQ,
                                   CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER, &cap);
        if (rc != 0)
        {
            k.irq_refs[i] = 0;
            k.irq_bindings.free(obj);
#if KICKOS_KERNEL_CORES > 1
            pub_unreserve(pub);
#endif
            return rc;
        }
        // The ISR is handed the binding's ADDRESS, stable for the slot's life.
#if KICKOS_KERNEL_CORES > 1
        pub_commit(pub, line, irq_event_isr, b);
#else
        irq_attach(line, irq_event_isr, b);
#endif
        // The line stays masked until the first irq_wait arms it.
        *out_cap = cap;
        return 0;
    }

    namespace
    {
        // Decode a signaller link; return null for the sentinel or an invalid slot.
        // Valid links are in [1, KICKOS_MAX_IRQ_HANDLES].
        // Stop on an invalid link rather than panic in the interrupt-masked wait path.
        // The null check also prevents GCC from adding an abort call on RX.
        IrqBinding* signaller_at(uint8_t ref)
        {
            if (ref == NOTIFY_SIGNALLER_NONE)
            {
                return nullptr;
            }
            return kernel().irq_bindings.at(notify_signaller_index(ref));
        }

        IrqBinding* signaller_first(Notification const* n)
        {
            return signaller_at(n->signallers);
        }

        IrqBinding* signaller_next(IrqBinding const* b)
        {
            return signaller_at(b->next_signaller);
        }
    }

    int irq_bind_notify(Thread* c, uint32_t irq_cap, uint32_t notify_cap)
    {
        IrqLock lock;
        int err = 0;
        IrqBinding* const b = binding_of_cap(c, irq_cap, CAP_WAIT, &err);
        if (b == nullptr)
        {
            return -err; // EBADF (bad/closed cap, freed slot) or EPERM (no WAIT right)
        }
        // The attachment lasts until the binding is released, keeping the ISR pointer valid.
        if (b->notify != nullptr)
        {
            return -KOS_EALREADY;
        }
        Notification* const n = static_cast<Notification*>(
            cap_resolve_e(c, notify_cap, CapType::CAP_NOTIFY, CAP_SIGNAL, &err));
        if (n == nullptr)
        {
            return -err;
        }
        CapEntry const* const ne = cap_lookup(c, notify_cap);
        int const obj = static_cast<int>(ne->obj);
        uint8_t const stored = cap_badge(*ne);
        uint8_t bit = 0;
        if (stored != KCAP_BADGE_NONE)
        {
            bit = static_cast<uint8_t>(stored - 1u);
        }
        int const idx = kernel().irq_bindings.index_of(b);
        if (idx < 0)
        {
            return -KOS_EBADF;
        }
#if KICKOS_KERNEL_CORES > 1
        // All signallers for a notification must route to the same core.
        int const dev_core = arch_irq_line_core(b->line);
        for (IrqBinding const* on = signaller_first(n); on != nullptr;
             on = signaller_next(on))
        {
            int const other = arch_irq_line_core(on->line);
            if (other >= 0 and dev_core >= 0 and other != dev_core)
            {
                return -KOS_EPERM;
            }
        }
#endif
        // Signallers may share a badge bit; rearming visits every matching line.
        // Check all tasks holding this IRQ before adding the notification to their holds.
        if (not task_object_admit_binding_notify(idx, obj))
        {
            return -KOS_EOVERFLOW;
        }
        // The binding retains the notification independently of its capabilities.
        if (not obj_ref_inc(CapType::CAP_NOTIFY, obj, 0))
        {
            return -KOS_EOVERFLOW;
        }
        b->badge = bit;
        b->notify = n;
        b->next_signaller = n->signallers;
        n->signallers = notify_signaller_ref(idx);
        return 0;
    }

    void irq_signallers_rearm(Notification* n, uint32_t mask)
    {
        for (IrqBinding* b = signaller_first(n); b != nullptr; b = signaller_next(b))
        {
            if ((mask & (1u << b->badge)) != 0u)
            {
                rearm_locked(b);
            }
        }
    }

    void irq_signallers_owe_rearm(Notification* n, uint32_t taken)
    {
        for (IrqBinding* b = signaller_first(n); b != nullptr; b = signaller_next(b))
        {
            if ((taken & (1u << b->badge)) != 0u)
            {
                b->needs_rearm = true;
            }
        }
    }

    void irq_unchain_signaller(int index)
    {
        IrqBinding* const b = kernel().irq_bindings.at(index);
        if (b == nullptr or b->notify == nullptr)
        {
            return;
        }
        uint8_t const self = notify_signaller_ref(index);
        Notification* const n = b->notify;
        // The head is its own case because it lives in the OBJECT and not in a binding, which
        // is why this cannot be one pointer-to-link walk over the whole chain.
        if (n->signallers == self)
        {
            n->signallers = b->next_signaller;
        }
        else
        {
            for (IrqBinding* p = signaller_first(n); p != nullptr; p = signaller_next(p))
            {
                if (p->next_signaller == self)
                {
                    p->next_signaller = b->next_signaller;
                    break;
                }
            }
        }
        b->next_signaller = NOTIFY_SIGNALLER_NONE;
    }

    void irq_detach_notify(int index)
    {
        IrqBinding* const b = kernel().irq_bindings.at(index);
        if (b == nullptr or b->notify == nullptr)
        {
            return;
        }
        Notification* const n = b->notify;
        b->notify = nullptr;
        int const idx = kernel().notifies.index_of(n);
        if (idx >= 0)
        {
            notify_ref_drop(kernel().notifies.handle_for(idx), true);
        }
    }

    int irq_pin_to_chain_core(Thread* c, uint32_t cap_handle)
    {
#if KICKOS_KERNEL_CORES > 1
        uint32_t effective = 0;
        {
            IrqLock lock;
            int err = 0;
            Notification const* const n = static_cast<Notification*>(
                cap_resolve_e(c, cap_handle, CapType::CAP_NOTIFY, CAP_WAIT, &err));
            if (n == nullptr)
            {
                return 0; // the wait itself reports the refusal, with its own taxonomy
            }
            int dev_core = -1;
            for (IrqBinding const* on = signaller_first(n); on != nullptr;
                 on = signaller_next(on))
            {
                int const one = arch_irq_line_core(on->line);
                if (one >= 0)
                {
                    dev_core = one; // irq_bind_notify refuses a chain that spans two
                }
            }
            if (dev_core < 0)
            {
                return 0;
            }
            int const arc = sched_admit_mask(1u << static_cast<unsigned>(dev_core),
                                             task_core_set(c->task), MaskBound::SUBSET,
                                             &effective);
            if (arc != 0)
            {
                return arc;
            }
        }
        // OUTSIDE the lock: set_affinity may reschedule.
        sched::set_affinity(c, effective);
#else
        (void)c;
        (void)cap_handle;
#endif
        return 0;
    }

    int irq_ack(Thread* c, uint32_t cap_handle)
    {
        int const prc = pin_to_line_core(c, cap_handle);
        if (prc != 0)
        {
            return prc;
        }
        IrqLock lock;
        int err = 0;
        IrqBinding* b = binding_of_cap(c, cap_handle, CAP_WAIT, &err);
        if (b == nullptr)
        {
            return -err; // EBADF (bad/closed cap) or EPERM (no WAIT right)
        }
        // A line that signals nothing must never be opened: the raise would land nowhere.
        if (b->notify == nullptr)
        {
            return -KOS_EINVAL;
        }
        // Optional and idempotent: a double ack, or an ack after auto-rearm, is a no-op.
        rearm_locked(b);
        return 0;
    }

    int irq_discard(Thread* c, uint32_t cap_handle)
    {
        int const prc = pin_to_line_core(c, cap_handle);
        if (prc != 0)
        {
            return prc;
        }
        IrqLock lock;
        int err = 0;
        IrqBinding* b = binding_of_cap(c, cap_handle, CAP_WAIT, &err);
        if (b == nullptr)
        {
            return -err; // EBADF (bad/closed cap) or EPERM (no WAIT right)
        }
        // The controller only: needs_rearm and the mask state are both untouched, so a
        // discard can neither arm nor open a line. Discarding an armed line races the
        // device; the latch is only known stale between a wait return and its ack.
        irq_line_op(b->line, LineOp::CLEAR);
        return 0;
    }

    void irq_ref_drop(int obj_handle, bool teardown)
    {
        (void)teardown; // no arm left reads it: nothing here can strand a waiter
        Kernel& k = kernel();
        IrqBinding* b = k.irq_bindings.resolve(obj_handle);
        int const idx = k.irq_bindings.index_of(b);
        if (idx < 0)
        {
            return;
        }
        uint8_t& r = k.irq_refs[idx];
        if (r > 0)
        {
            r--;
        }
        if (r == 0)
        {
            // NO leak-don't-strand arm and none owed: a waiter is parked on the OBJECT, not
            // on this line, and the object's own reference count keeps it alive. Dropping the
            // last capability naming this line while a driver waits strands nobody; it takes
            // the line away, which is what closing that capability says.
            //
            // Unchained HERE and released at the slot free below: a rearm must not reach a
            // line that is going away, but a dispatch on another core may already hold this
            // binding's object pointer and must find the object still there.
            irq_unchain_signaller(idx);
            // The budget comes back HERE and not at the pool free below, which above one
            // kernel core happens later, from a reclamation: the binding is unreachable from
            // this instant and holding its owner until the slot returns would keep charging a
            // task for a line it no longer has.
#if KICKOS_KERNEL_CORES > 1
            // The slot returns with the record's grace period: a dispatch still reading that
            // record is one still holding this slot's address as its pre-bound argument.
            line_release(b->line, obj_handle);
#else
            // DETACH BEFORE FREE: irq_event_isr holds this binding's address as its
            // pre-bound arg, so the slot must leave the dispatch table before it returns
            // to the pool. The detach also masks the line and restores the null-object,
            // which is what lets a later irq_claim of the same line pass its EBUSY test.
            irq_detach(b->line);
            irq_detach_notify(idx);
            k.irq_bindings.free(obj_handle);
#endif
        }
    }
}

// ISR context. `irq` is the device line that fired.
extern "C" void kickos_isr_irq(int irq)
{
    if (irq < 0 or irq >= KICKOS_MAX_IRQ)
    {
        return;
    }
#if KICKOS_KERNEL_CORES > 1
    // Odd from before the entry is read to after the handler returns; a retirement on another
    // core reads this to tell that this core has left the entry.
    uint32_t const me = kickos_kernel_core();
    ::kickos::EpochRow& row = ::kickos::g_epoch_row[me];
    if (row.depth == 0)
    {
        row.epoch = row.epoch.load() + 1u;
        ::kickos::epoch_fence();
    }
    row.depth = row.depth + 1u;
    // No null check: every publication is a valid callback (the null-object default).
    ::kickos::IrqDispatch const d = ::kickos::irq_published(irq);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_enter(static_cast<uint16_t>(irq));
#endif
    d.handler(d.arg);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_exit(static_cast<uint16_t>(irq));
#endif
    row.depth = row.depth - 1u;
    if (row.depth == 0)
    {
        row.epoch = row.epoch.load() + 1u;
    }
#else
    ::kickos::Kernel& k = ::kickos::kernel();
    // No null check: every slot is a valid callback (the null-object default).
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_enter(static_cast<uint16_t>(irq));
#endif
    k.irq_table[irq].handler(k.irq_table[irq].arg);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_exit(static_cast<uint16_t>(irq));
#endif
#endif
}
