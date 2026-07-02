// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RAII critical section over the arch irq-save/restore seam. Nesting-safe:
// each lock records the prior state and restores exactly that on scope exit.

#ifndef KICKOS_IRQLOCK_H
#define KICKOS_IRQLOCK_H

#include <kickos/arch/arch.h>

namespace kickos
{

    class IrqLock
    {
    public:
        IrqLock()
            : state_(arch_irq_save())
        {
        }
        ~IrqLock() { arch_irq_restore(state_); }

        IrqLock(IrqLock const&) = delete;
        IrqLock& operator=(IrqLock const&) = delete;

    private:
        arch_irq_state_t state_;
    };

}

#endif
