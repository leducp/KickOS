// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/irq.h>
#include <kickos/instance.h>
#include <kickos/config.h>
#include <kickos/sync.h>
#include <kickos/irqlock.h>
#include <kickos/cap.h>
#include <kickos/kernel.h> // KICKOS_ASSERT
#include <kickos/arch/arch.h>
#include <kickos/ktrace.h>

#include <kickos/sys/abi.h>   // KOS_IRQ_LEVEL claim flag
#include <kickos/sys/errno.h>

namespace kickos
{
    namespace
    {
        // Resolve a caller's CAP_IRQ to its binding, enforcing `need` at the one
        // rights chokepoint. Caller holds IrqLock.
        IrqBinding* binding_of_cap(Thread* c, int cap_handle, uint8_t need, int* err)
        {
            return static_cast<IrqBinding*>(
                cap_resolve_e(c, cap_handle, CapType::CAP_IRQ, need, err));
        }

        // Rearm the line a previous wait consumed. Caller holds IrqLock.
        // EDGE is a bare unmask: a raise latched while masked redelivers, so no pulse is
        // lost. The FIRST arm discards the latch whatever the trigger, since a raise latched
        // before the line had an owner would phantom-wake the first wait; LEVEL keeps
        // discarding on every rearm, after the driver's own device clear. Clearing here for
        // EDGE would drop the coalesced raises this path exists to redeliver, so an EDGE
        // driver that KNOWS its latch is stale calls irq_discard instead.
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

        // First-level ISR stub for a tier-1 line: mask, so it cannot re-fire while the
        // driver services it, then post the bound notification. `arg` IS the pre-bound
        // binding: no table lookup in ISR context (the latency invariant).
        void irq_event_isr(void* arg)
        {
            IrqBinding* b = static_cast<IrqBinding*>(arg);
            arch_irq_mask(b->line);
            sem_post(&b->sem);
        }

        // Null-object default bound to every line with no driver. An enabled line that fires
        // with no handler must be masked, or it re-asserts forever into an interrupt storm,
        // and counted rather than silently dropped. ISR context, so async-safe only: mask and
        // bump a counter, no I/O. `arg` encodes the line (seeded by irq_init/irq_detach).
        void irq_default_handler(void* arg)
        {
            int line = static_cast<int>(reinterpret_cast<intptr_t>(arg));
            arch_irq_mask(line);
            kernel().irq_spurious_count++;
        }

        void set_default(int irq)
        {
            kernel().irq_table[irq].handler = irq_default_handler;
            kernel().irq_table[irq].arg =
                reinterpret_cast<void*>(static_cast<intptr_t>(irq));
        }
    }

    // Seed every line with the null-object default so the dispatch table has no
    // null slots. Must run before any irq_attach/irq_register (kmain, pre-start).
    void irq_init()
    {
        IrqLock lock;
        kernel().irq_spurious_count = 0;
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
        // One driver per line: only a line still holding the null-object default
        // is free to claim. Without this a tier-2 attach would silently overwrite
        // a line a tier-1 driver already owns, orphaning its irq_wait() forever.
        if (kernel().irq_table[irq].handler != irq_default_handler)
        {
            return false;
        }
        kernel().irq_table[irq].handler = handler;
        kernel().irq_table[irq].arg = arg;
        return true;
    }

    void irq_detach(int irq)
    {
        if (irq < 0 or irq >= KICKOS_MAX_IRQ)
        {
            return;
        }
        IrqLock lock;
        set_default(irq); // restore the null-object, not a null slot
        arch_irq_mask(irq); // register/attach armed the line; detach disarms it
    }

    int irq_claim(Thread* c, int line, unsigned int flags)
    {
        IrqLock lock;
        if (c == nullptr)
        {
            return -KOS_EPERM; // no caller context (defensive; unreachable from a real syscall)
        }
        if (line < 0 or line >= KICKOS_MAX_IRQ)
        {
            return -KOS_EINVAL; // bad irq line
        }
        if ((flags & ~static_cast<unsigned int>(KOS_IRQ_LEVEL)) != 0)
        {
            return -KOS_EINVAL; // unknown claim flag
        }
        Kernel& k = kernel();
        // One driver per line: a line is free iff it still holds the null-object
        // default (every slot is non-null since irq_init, so the old != nullptr
        // check would reject every line). This is also what enforces the ordering
        // in INVARIANT H2: the console line stays unclaimable until the kernel's
        // own console_tx_deinit has detached it.
        if (k.irq_table[line].handler != irq_default_handler)
        {
            return -KOS_EBUSY; // line already owned; no stealing
        }
        int const i = k.irq_bindings.alloc();
        if (i < 0)
        {
            return -KOS_ENOMEM; // binding pool exhausted
        }
        IrqBinding* b = k.irq_bindings.at(i);
        sem_init(&b->sem, 0);
        b->line = line;
        b->needs_rearm = true; // the first irq_wait arms the line (INVARIANT H1)
        b->armed_once = false;
        b->trigger = IRQ_EDGE;
        if ((flags & static_cast<unsigned int>(KOS_IRQ_LEVEL)) != 0)
        {
            b->trigger = IRQ_LEVEL;
        }
        k.irq_refs[i] = 1; // this claimer's cap is the first reference
        int const obj = k.irq_bindings.handle_for(i);
        // A full table is a clean failure: release the just-claimed binding. Install
        // BEFORE attaching so a failure path never leaves a line bound to a slot that
        // is about to be freed.
        int const cap = cap_install(c, obj, CapType::CAP_IRQ,
                                    CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER);
        if (cap < 0)
        {
            k.irq_refs[i] = 0;
            k.irq_bindings.free(obj);
            return -KOS_ENOMEM; // cap_install returns a bare -1, which reads as -KOS_EPERM
        }
        // The ISR is handed the binding's ADDRESS, stable for the slot's life.
        irq_attach(line, irq_event_isr, b);
        // Deliberately NOT armed here. The line stays masked until the first irq_wait, so it
        // is never armed while its eventual owner is not yet running, which is what closes
        // the publish-to-claim window.
        return cap;
    }

    // Is `t` parked inside irq_wait right now? A parked waiter pins its binding through its
    // own cap, so a match can never name a slot that has been freed under it.
    //
    // thread_kill needs this because a wait queue does not say what it delivers: only THIS
    // park reads wait_result, and sem_wait ignores it, so an early wake on a plain semaphore
    // would read as a token that was never handed over.
    bool irq_thread_parked(Thread const* t)
    {
        if (t == nullptr or t->wait_queue == nullptr)
        {
            return false;
        }
        Kernel& k = kernel();
        for (int i = 0; i < k.irq_bindings.capacity(); i++)
        {
            if (t->wait_queue == &k.irq_bindings.at(i)->sem.waiters)
            {
                return true;
            }
        }
        return false;
    }

    // The ONE cancellation point in the kernel. It must NOT be folded back into sem_wait:
    // sem_wait returns void and never reads wait_result, so a third party waking it early
    // would be indistinguishable from a post. Parking through wq_block directly is what
    // gives this wait an error return.
    int irq_wait(Thread* c, int cap_handle)
    {
        IrqBinding* b = nullptr;
        uint64_t epoch = 0;
        {
            IrqLock lock;
            int err = 0;
            b = binding_of_cap(c, cap_handle, CAP_WAIT, &err);
            if (b == nullptr)
            {
                return -err; // EBADF (bad/closed cap, freed slot) or EPERM (no WAIT right)
            }
            if (c->cancelled)
            {
                return -KOS_ECANCELED; // refuse to RE-block a thread already cancelled
            }
            // Arm the line: the first wait does the initial arm, every later one
            // rearms what the previous wait consumed (no explicit ack needed).
            rearm_locked(b);
            if (b->sem.count > 0)
            {
                // A raise already banked: consume it without parking. Event consumed, so
                // the same rearm flag the parked path sets on resume.
                b->sem.count--;
                b->needs_rearm = true;
                return 0;
            }
            // A SlotPool slot address is stable for the slot's life and this waiter holds
            // a reference to it (its own cap), so `b` stays valid across the park: the
            // binding cannot be freed under us.
            c->wait_result = 0; // sem_post hands the token WITHOUT writing this
            epoch = c->switch_count;
            wq_block(b->sem.waiters);
        }
        // Mandatory, and OUTSIDE the lock: on ARM the switch is only pended when the
        // block scope's lock drops, so a wait_result read before this returns the
        // pre-block value (sync.h).
        wq_confirm_resume(c, epoch);
        {
            IrqLock lock;
            if (c->wait_result != 0)
            {
                // Cancelled. The line is deliberately left as the rearm above set it:
                // this thread is exiting, and its cap drop detaches and masks the line.
                return static_cast<int>(c->wait_result);
            }
            // Event consumed: flag the line for rearm HERE, not in the ISR. The ISR
            // masked before posting, so the line is masked now; setting the flag on
            // wait-return (never in ISR) is what makes ack;compute;wait phantom-free.
            b->needs_rearm = true;
        }
        return 0;
    }

    int irq_ack(Thread* c, int cap_handle)
    {
        IrqLock lock;
        int err = 0;
        IrqBinding* b = binding_of_cap(c, cap_handle, CAP_WAIT, &err);
        if (b == nullptr)
        {
            return -err; // EBADF (bad/closed cap) or EPERM (no WAIT right)
        }
        // OPTIONAL + idempotent: only unmask if a wait consumed an event and left
        // the line masked. A double ack, or an ack after auto-rearm, is a no-op.
        rearm_locked(b);
        return 0;
    }

    int irq_discard(Thread* c, int cap_handle)
    {
        IrqLock lock;
        int err = 0;
        IrqBinding* b = binding_of_cap(c, cap_handle, CAP_WAIT, &err);
        if (b == nullptr)
        {
            return -err; // EBADF (bad/closed cap) or EPERM (no WAIT right)
        }
        // The controller only: needs_rearm and the mask state are both untouched, so a
        // discard can neither arm nor open a line and the sequencing stays with the driver.
        // Discarding while the line is still armed is legal and races the device by
        // construction; the caller can only know the latch is stale with the line masked,
        // that is, between a wait return and its ack.
        arch_irq_clear_pending(b->line);
        return 0;
    }

    int irq_notify(Thread* c, int cap_handle)
    {
        IrqLock lock;
        int err = 0;
        IrqBinding* b = binding_of_cap(c, cap_handle, CAP_SIGNAL, &err);
        if (b == nullptr)
        {
            return -err; // EBADF (bad/closed cap) or EPERM (no SIGNAL right)
        }
        // The controller is NOT touched: the waiter wakes with nothing asserted and must be
        // idempotent about finding no work. needs_rearm is left alone, since a notify must
        // not unmask a line whose event has not been serviced.
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
            // Same leak-don't-strand guard as the sem/mutex/endpoint arms, and UNREACHABLE:
            // every parked waiter holds its own cap, so waiters <= refs, and cancellation
            // unlinks the target in the killer's context before the target tears down. An
            // async destroy would break that, and this assert is what would say so.
            if (not b->sem.waiters.empty())
            {
                KICKOS_ASSERT(teardown); // refs->0 with a waiter parked is unreachable via close
                r = 1;                   // leak, never strand
                return;
            }
            // DETACH BEFORE FREE, and it is load-bearing: irq_event_isr holds this
            // binding's address as its pre-bound arg, so the slot must stop being
            // reachable from the dispatch table before it returns to the pool.
            // irq_detach restores the null-object default AND masks the line, which is
            // what makes a later irq_claim of the same line pass its EBUSY test.
            irq_detach(b->line);
            k.irq_bindings.free(obj_handle);
        }
    }
}

// Called by the arch backend in ISR context when device line `irq` fires. Runs
// the attached handler by index (no search); the handler posts a sem/flag and so
// drives a switch to the readied thread on interrupt exit.
extern "C" void kickos_isr_irq(int irq)
{
    if (irq < 0 or irq >= KICKOS_MAX_IRQ)
    {
        return;
    }
    ::kickos::Kernel& k = ::kickos::kernel();
    // Every slot is a valid callback (the null-object default), so no null check:
    // an unbound line dispatches to irq_default_handler (mask + spurious count).
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_enter(static_cast<uint16_t>(irq));
#endif
    k.irq_table[irq].handler(k.irq_table[irq].arg);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_exit(static_cast<uint16_t>(irq));
#endif
}
