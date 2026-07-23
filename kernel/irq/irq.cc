// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/irq.h>
#include <kickos/instance.h>
#include <kickos/config.h>
#include <kickos/sync.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>
#include <kickos/ktrace.h>

#include <kickos/sys/errno.h>

namespace kickos
{
    namespace
    {
        IrqBinding* binding_of(int handle)
        {
            Kernel& k = kernel();
            if (handle < 0 or handle >= k.irq_binding_count)
            {
                return nullptr;
            }
            IrqBinding& b = k.irq_bindings[handle];
            if (not b.used)
            {
                return nullptr;
            }
            return &b;
        }

        // First-level ISR stub for a tier-1 line: mask (so it can't re-fire while
        // the driver services it), then post the bound notification. Runs in ISR
        // context; `arg` IS the pre-bound binding -- no table lookup here (the
        // latency invariant).
        void irq_event_isr(void* arg)
        {
            IrqBinding* b = static_cast<IrqBinding*>(arg);
            arch_irq_mask(b->line);
            sem_post(&b->sem);
        }

        // Null-object default bound to every line with no driver. An enabled line
        // that fires with no handler must be masked (else it re-asserts forever ->
        // interrupt storm) and counted, never silently dropped. Being the default
        // for every slot removes the hot-path null check in kickos_isr_irq. Runs
        // in ISR context, so async-safe only: mask + bump a counter, no I/O (the
        // count is surfaced out of ISR context via irq_spurious_count()). `arg`
        // encodes the line (seeded by irq_init/irq_detach).
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

    int irq_register(int line)
    {
        IrqLock lock;
        if (line < 0 or line >= KICKOS_MAX_IRQ)
        {
            return -KOS_EINVAL; // bad irq line
        }
        Kernel& k = kernel();
        // One driver per line: a line is free iff it still holds the null-object
        // default (every slot is non-null since irq_init, so the old != nullptr
        // check would reject every line).
        if (k.irq_table[line].handler != irq_default_handler)
        {
            return -KOS_EBUSY; // line already owned -- no stealing
        }
        if (k.irq_binding_count >= KICKOS_MAX_IRQ_HANDLES)
        {
            return -KOS_ENOMEM; // binding pool exhausted
        }
        // Bump-allocated: no unregister/free path yet (freelist deferred, like the
        // thread pool -- the sem pool's generational SlotPool (slotpool.h) is the pattern).
        int handle = k.irq_binding_count++;
        IrqBinding& b = k.irq_bindings[handle];
        sem_init(&b.sem, 0);
        b.line = line;
        b.used = true;
        irq_attach(line, irq_event_isr, &b);
        // Arm the line for its first IRQ. Without this the line is delivered only
        // after the first irq_ack -- fine on controllers unmasked-by-default
        // (sim/riscv) but a deadlock on default-masked ones (ARM NVIC, RX): the
        // first raise only latches on the masked line and never fires, so the wait
        // never wakes and ack never runs.
        //
        // Clear BEFORE enabling: any raise latched on the line before a driver
        // owned it is pre-registration garbage; the latch-and-coalesce contract
        // would otherwise redeliver it and phantom-wake the very first irq_wait.
        // INVARIANT: a latched raise preserved across unmask redelivers via the
        // normal ISR path (irq_event_isr masks + posts, needs_rearm is set only on
        // wait-return) -- unmask never sem_posts directly.
        arch_irq_clear_pending(line);
        arch_irq_unmask(line);
        return handle;
    }

    int irq_wait(int handle)
    {
        IrqBinding* b = nullptr;
        {
            IrqLock lock;
            b = binding_of(handle);
            if (b == nullptr)
            {
                return -KOS_EBADF; // bad irq handle
            }
            // Auto-rearm the line consumed by the previous wait (no explicit ack
            // needed). needs_rearm starts false, so the first wait just blocks.
            if (b->needs_rearm)
            {
                b->needs_rearm = false;
                arch_irq_unmask(b->line);
            }
        }
        // The binding is stable (bindings are never moved or freed in M0.2), so
        // parking on its sem outside the lock is safe.
        sem_wait(&b->sem);
        // Event consumed: flag the line for rearm HERE, not in the ISR. The ISR
        // masked before posting, so the line is masked now; setting the flag on
        // wait-return (never in ISR) is what makes ack;compute;wait phantom-free.
        {
            IrqLock lock;
            b->needs_rearm = true;
        }
        return 0;
    }

    int irq_ack(int handle)
    {
        IrqLock lock;
        IrqBinding* b = binding_of(handle);
        if (b == nullptr)
        {
            return -KOS_EBADF; // bad irq handle
        }
        // OPTIONAL + idempotent: only unmask if a wait consumed an event and left
        // the line masked. A double ack, or an ack after auto-rearm, is a no-op.
        if (b->needs_rearm)
        {
            b->needs_rearm = false;
            arch_irq_unmask(b->line);
        }
        return 0;
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
