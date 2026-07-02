// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/irq.h>
#include <kickos/config.h>
#include <kickos/irqlock.h>

namespace kickos
{
    namespace
    {

        struct Entry
        {
            IrqHandler handler = nullptr;
            void* arg = nullptr;
        };

        Entry g_table[KICKOS_MAX_IRQ];

    }

    void irq_attach(int irq, IrqHandler handler, void* arg)
    {
        if (irq < 0 || irq >= KICKOS_MAX_IRQ) return;
        IrqLock lock;
        g_table[irq].handler = handler;
        g_table[irq].arg = arg;
    }

    void irq_detach(int irq)
    {
        if (irq < 0 || irq >= KICKOS_MAX_IRQ) return;
        IrqLock lock;
        g_table[irq].handler = nullptr;
        g_table[irq].arg = nullptr;
    }

}

// Called by the arch backend in ISR context when device line `irq` fires.
// Runs the attached privileged handler, which typically posts a sem/flag and
// so drives a switch to the readied thread on interrupt exit.
extern "C" void kickos_isr_irq(int irq)
{
    if (irq < 0 || irq >= KICKOS_MAX_IRQ) return;
    ::kickos::IrqHandler h = ::kickos::g_table[irq].handler;
    void* arg = ::kickos::g_table[irq].arg;
    if (h != nullptr) h(arg);
}
