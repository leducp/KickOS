// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What kernel/init/console_tx.cc leaves undefined that every gate arming the ring through
// console_tx_init answers alike.

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/irq.h>

extern "C"
{
    int arch_in_isr(void) { return 0; }
    void arch_irq_mask(int) {}
    void arch_irq_unmask(int) {}
    void arch_irq_clear_pending(int) {}
    console_tx_backend const* arch_console_tx_backend(int*) { return nullptr; }
}

namespace kickos
{
    bool irq_attach(int, IrqHandler, void*) { return true; }
    void irq_detach(int) {}
}
