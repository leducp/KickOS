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
    }

    void irq_attach(int irq, IrqHandler handler, void* arg)
    {
        if (irq < 0 or irq >= KICKOS_MAX_IRQ)
        {
            return;
        }
        IrqLock lock;
        kernel().irq_table[irq].handler = handler;
        kernel().irq_table[irq].arg = arg;
    }

    void irq_detach(int irq)
    {
        if (irq < 0 or irq >= KICKOS_MAX_IRQ)
        {
            return;
        }
        IrqLock lock;
        kernel().irq_table[irq] = IrqEntry{};
    }

    int irq_register(int line)
    {
        IrqLock lock;
        if (line < 0 or line >= KICKOS_MAX_IRQ)
        {
            return -1;
        }
        Kernel& k = kernel();
        if (k.irq_table[line].handler != nullptr) // one driver per line
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
    ::kickos::IrqHandler h = k.irq_table[irq].handler;
    void* arg = k.irq_table[irq].arg;
    if (h != nullptr)
    {
        h(arg);
    }
}
