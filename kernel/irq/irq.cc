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
            kernel().irq_table[irq].handler = irq_default_handler;
            kernel().irq_table[irq].arg =
                reinterpret_cast<void*>(static_cast<intptr_t>(irq));
        }
    }

    // Must run before any irq_attach/irq_claim (kmain, pre-start): after it no slot is null.
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
        // One driver per line: only a line still holding the null-object default is free.
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
        set_default(irq); // the null-object, not a null slot
        arch_irq_mask(irq);
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
        // One driver per line: free iff it still holds the null-object default. This is also
        // what enforces INVARIANT H2: the console line stays unclaimable until the kernel's
        // own console_tx_deinit has detached it.
        if (k.irq_table[line].handler != irq_default_handler)
        {
            return -KOS_EBUSY;
        }
        int const i = k.irq_bindings.alloc();
        if (i < 0)
        {
            return -KOS_ENOMEM;
        }
        IrqBinding* b = k.irq_bindings.at(i);
        sem_init(&b->sem, 0);
        b->line = line;
        // The first irq_wait arms the line (INVARIANT H1): a claim leaves it masked, so
        // there is no window in which the line is armed and unowned.
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
            return rc;
        }
        // The ISR is handed the binding's ADDRESS, stable for the slot's life.
        irq_attach(line, irq_event_isr, b);
        // Deliberately NOT armed here (INVARIANT H1).
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
            // DETACH BEFORE FREE: irq_event_isr holds this binding's address as its
            // pre-bound arg, so the slot must leave the dispatch table before it returns
            // to the pool. The detach also masks the line and restores the null-object,
            // which is what lets a later irq_claim of the same line pass its EBUSY test.
            irq_detach(b->line);
            k.irq_bindings.free(obj_handle);
        }
    }
}

// Called by the arch backend in ISR context when device line `irq` fires.
extern "C" void kickos_isr_irq(int irq)
{
    if (irq < 0 or irq >= KICKOS_MAX_IRQ)
    {
        return;
    }
    ::kickos::Kernel& k = ::kickos::kernel();
    // No null check: every slot is a valid callback (the null-object default).
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_enter(static_cast<uint16_t>(irq));
#endif
    k.irq_table[irq].handler(k.irq_table[irq].arg);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_exit(static_cast<uint16_t>(irq));
#endif
}
