// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_IRQ_ROUTE_H
#define KICKOS_IRQ_ROUTE_H

#include <kickos/arch/arch.h>
#include <kickos/config.h>
#include <kickos/held.h>

namespace kickos
{
    // Delivery gating for one logical line.
    enum class LineOp : unsigned char
    {
        MASK,
        UNMASK,
        CLEAR
    };

    // The only way the kernel layer may gate a line: performs the operation on the core the
    // line is routed to, through the doorbell rendezvous when that is not this core
    // (docs/design-multicore.md N3). tests/static/check_irq_line_op_sole.sh refuses a
    // kernel-layer call to arch_irq_mask, arch_irq_unmask or arch_irq_clear_pending
    // anywhere else.
    void irq_line_op(int line, LineOp op, Held held);

    // The kernel layer's one way to raise a line in software; the same gate refuses a
    // kernel-layer call to arch_irq_inject anywhere else.
    inline void irq_inject(int line, Held)
    {
        arch_irq_inject(line);
    }

    // For a caller that IS the routed core by construction, which here means ISR context
    // alone. Reaches no rendezvous, which keeps the dispatch's callgraph inside the red zone
    // check_trap_redzone.sh enforces.
    void irq_line_op_local(int line, LineOp op);

#if KICKOS_KERNEL_CORES > 1
    // Drains this core's asks. Called from the doorbell SERVICE BODY and nowhere else, AFTER it
    // has snapshotted the request sequences and BEFORE it stores the answers: a drain merely
    // ahead of the answer stores answers work it never did. The service's step types order it
    // (arch/common/doorbell_protocol.cc), and tests/static/check_route_service_order.sh reads
    // that the drain step is its caller.
    // extern "C" because the arch layer is its only caller and carries no kernel include path.
    extern "C" void kickos_irq_route_service(void);
#endif
}

#endif // KICKOS_IRQ_ROUTE_H
