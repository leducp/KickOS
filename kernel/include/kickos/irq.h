// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Interrupts as events that wake a thread. Two tiers:
//   Tier 2 (privileged, in-kernel): irq_attach binds a direct handler that runs
//     in ISR context and typically posts a sem/flag.
//   Tier 1 (unprivileged userspace driver): irq_register/wait/ack. A handle binds
//     a line to a notification; the generic first-level ISR masks the line and
//     posts it, the driver waits in thread context, services, and acks (unmask).

#ifndef KICKOS_IRQ_H
#define KICKOS_IRQ_H

#include <kickos/sync.h>

namespace kickos
{
    using IrqHandler = void (*)(void* arg);

    // Line -> handler dispatch entry; the ISR reads it by index, never a search.
    struct IrqEntry
    {
        IrqHandler handler = nullptr;
        void* arg = nullptr;
    };

    // Tier 1 binding: a line + the notification the driver waits on. The ISR is
    // handed this binding directly as its arg (the pre-bound target), so it never
    // searches a table to find the sem in ISR context -- the latency invariant.
    struct IrqBinding
    {
        Semaphore sem;
        int line = -1;
        bool used = false;
    };

    // Tier 2: privileged in-kernel direct handler.
    void irq_attach(int irq, IrqHandler handler, void* arg);
    void irq_detach(int irq);

    // Tier 1: IRQ-as-event (usable from unprivileged userspace via syscalls).
    int irq_register(int line); // -> handle, or -1
    int irq_wait(int handle);   // block until the line fires; 0, or -1 on bad handle
    int irq_ack(int handle);    // unmask the line so it can fire again; 0, or -1
}

#endif
