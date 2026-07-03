// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/irq.h>
#include <kickos/instance.h>
#include <kickos/config.h>
#include <kickos/sync.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>

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
    }

    int irq_register(int line)
    {
        IrqLock lock;
        if (line < 0 or line >= KICKOS_MAX_IRQ)
        {
            return -1;
        }
        Kernel& k = kernel();
        // One driver per line: a line is free iff it still holds the null-object
        // default (every slot is non-null since irq_init, so the old != nullptr
        // check would reject every line).
        if (k.irq_table[line].handler != irq_default_handler)
        {
            return -1;
        }
        if (k.irq_binding_count >= KICKOS_MAX_IRQ_HANDLES)
        {
            return -1;
        }
        // Bump-allocated: no unregister/free path yet (freelist deferred, like the
        // thread pool -- see 8m for the pattern the sem pool already adopted).
        int handle = k.irq_binding_count++;
        IrqBinding& b = k.irq_bindings[handle];
        sem_init(&b.sem, 0);
        b.line = line;
        b.used = true;
        irq_attach(line, irq_event_isr, &b);
        return handle;
    }

    int irq_wait(int handle)
    {
        IrqBinding* b = nullptr;
        {
            IrqLock lock;
            b = binding_of(handle);
        }
        if (b == nullptr)
        {
            return -1;
        }
        // The binding is stable (bindings are never moved or freed in M0.2), so
        // parking on its sem outside the lock is safe.
        sem_wait(&b->sem);
        return 0;
    }

    int irq_ack(int handle)
    {
        IrqLock lock;
        IrqBinding* b = binding_of(handle);
        if (b == nullptr)
        {
            return -1;
        }
        arch_irq_unmask(b->line);
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
    k.irq_table[irq].handler(k.irq_table[irq].arg);
}
