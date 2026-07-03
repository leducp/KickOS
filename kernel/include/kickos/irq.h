// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// In-kernel interrupt attach (privileged direct handlers). An interrupt is an
// event that wakes a thread: the handler runs in ISR context, typically posts
// a semaphore/flag, and the scheduler switches to the readied thread on
// interrupt exit. The userspace irq-as-event API (irq_register/wait/ack) lands
// later; M0 exercises the in-kernel direct path.

#ifndef KICKOS_IRQ_H
#define KICKOS_IRQ_H

namespace kickos
{
    using IrqHandler = void (*)(void* arg);

    void irq_attach(int irq, IrqHandler handler, void* arg);
    void irq_detach(int irq);
}

#endif
