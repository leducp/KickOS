// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/irq.h>
#include <kickos/instance.h>
#include <kickos/config.h>
#include <kickos/sync.h>
#include <kickos/irqlock.h>
#include <kickos/cap.h>
#include <kickos/kernel.h> // KICKOS_ASSERT
#include <kickos/debug.h>  // KICKOS_DEBUG_ASSERT
#include <kickos/arch/arch.h>
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
        // Every record that is not free is named by a line, so a caller that has found a free
        // line has left a record free for it: pub_reserve cannot refuse a claim that got past
        // the line test. The refusal stays because this inequality is its only guarantee.
        static_assert(IRQ_PUB_SLOTS - 1 >= KICKOS_MAX_IRQ,
                      "fewer usable records than lines would make a free line unpublishable");
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
            b->needs_rearm = false;
            if (not b->armed_once or b->trigger == IRQ_LEVEL)
            {
                arch_irq_clear_pending(b->line);
            }
            b->armed_once = true;
            arch_irq_unmask(b->line);
        }

        // ISR context. `arg` is the pre-bound binding, not a line number.
        // Masks the line; the matching unmask is rearm_locked, on wait return.
        void irq_event_isr(void* arg)
        {
            IrqBinding* b = static_cast<IrqBinding*>(arg);
            arch_irq_mask(b->line);
            sem_post(&b->sem);
        }

        // Null-object default bound to every line with no driver. An unhandled enabled line
        // must be masked or it re-asserts forever. ISR context: mask and count, no I/O.
        // `arg` encodes the line (seeded by irq_init/irq_detach).
        void irq_default_handler(void* arg)
        {
            int line = static_cast<int>(reinterpret_cast<intptr_t>(arg));
            arch_irq_mask(line);
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
            arch_irq_mask(line);
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
        arch_irq_mask(irq);
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
        Kernel& k = kernel();
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
        if (i < 0)
        {
#if KICKOS_KERNEL_CORES > 1
            pub_unreserve(pub);
#endif
            return -KOS_ENOMEM;
        }
        IrqBinding* b = k.irq_bindings.at(i);
        sem_init(&b->sem, 0);
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

    // The ONE cancellation point in the kernel. Do NOT fold back into sem_wait: sem_wait
    // returns void and never reads wait_result, so an early wake would look like a post.
    int irq_wait(Thread* c, uint32_t cap_handle)
    {
        IrqBinding* b = nullptr;
        uint32_t epoch = 0;
        {
            IrqLock lock;
            int err = 0;
            b = binding_of_cap(c, cap_handle, CAP_WAIT, &err);
            if (b == nullptr)
            {
                return -err; // EBADF (bad/closed cap, freed slot) or EPERM (no WAIT right)
            }
            if (c->cancel_kind != CANCEL_NONE)
            {
                return -KOS_ECANCELED;
            }
            rearm_locked(b);
            if (b->sem.count > 0)
            {
                // Flag for rearm exactly as the parked path does on resume.
                b->sem.count--;
                b->needs_rearm = true;
                return 0;
            }
            // `b` survives the park: this waiter's own cap holds a reference to the slot.
            c->wait_result = 0; // sem_post hands the token WITHOUT writing this
            epoch = c->switch_count;
            // WAIT_IRQ, not WAIT_SEM, though the queue is a semaphore's: only this tag says
            // the park reads wait_result and so may be ended early.
            wq_block(b->sem.waiters, WAIT_IRQ, b);
        }
        // Mandatory, and OUTSIDE the lock: where the switch is only pended when the block
        // scope's lock drops, a wait_result read before this returns the pre-block value.
        wq_confirm_resume(c, epoch);
        {
            IrqLock lock;
            if (c->wait_result != 0)
            {
                // Left as the rearm above set it: the exiting thread's cap drop detaches
                // and masks the line.
                return static_cast<int>(c->wait_result);
            }
            // Flag for rearm HERE, never in the ISR: that is what makes ack;compute;wait
            // phantom-free.
            b->needs_rearm = true;
        }
        return 0;
    }

    int irq_ack(Thread* c, uint32_t cap_handle)
    {
        IrqLock lock;
        int err = 0;
        IrqBinding* b = binding_of_cap(c, cap_handle, CAP_WAIT, &err);
        if (b == nullptr)
        {
            return -err; // EBADF (bad/closed cap) or EPERM (no WAIT right)
        }
        // Optional and idempotent: a double ack, or an ack after auto-rearm, is a no-op.
        rearm_locked(b);
        return 0;
    }

    int irq_discard(Thread* c, uint32_t cap_handle)
    {
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
        arch_irq_clear_pending(b->line);
        return 0;
    }

    int irq_notify(Thread* c, uint32_t cap_handle)
    {
        IrqLock lock;
        int err = 0;
        IrqBinding* b = binding_of_cap(c, cap_handle, CAP_SIGNAL, &err);
        if (b == nullptr)
        {
            return -err; // EBADF (bad/closed cap) or EPERM (no SIGNAL right)
        }
        // The controller is NOT touched: the waiter wakes with nothing asserted and must be
        // idempotent about finding no work. A notify must never unmask an unserviced line.
        sem_post(&b->sem);
        return 0;
    }

    void irq_ref_drop(int obj_handle, bool teardown)
    {
        Kernel& k = kernel();
        IrqBinding* b = k.irq_bindings.resolve(obj_handle);
        if (b == nullptr)
        {
            return;
        }
        int const idx = static_cast<int>(b - k.irq_bindings.at(0));
        uint8_t& r = k.irq_refs[idx];
        if (r > 0)
        {
            r--;
        }
        if (r == 0)
        {
            if (not b->sem.waiters.empty())
            {
                KICKOS_ASSERT(teardown);
                r = 1; // leak, never strand
                return;
            }
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
