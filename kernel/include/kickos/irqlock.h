// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RAII critical section: this core's interrupts off, and above one core the cross-core
// kernel lock too. Nesting-safe in both halves: each instance records the prior interrupt
// state and restores exactly that, and the lock is taken as the per-core acquisition depth
// rises from zero and released as it returns to it (kickos/klock.h).
//
// THE ORDER IS PART OF THE CONTRACT: mask before taking the lock, release before unmasking.
// Reversed, a handler entered on a core holding the lock spins against its own release.

#ifndef KICKOS_IRQLOCK_H
#define KICKOS_IRQLOCK_H

#include <kickos/arch/arch.h>
#include <kickos/klock.h>

namespace kickos
{
    class IrqLock
    {
    public:
        // ALWAYS INLINE ABOVE ONE CORE: an out-of-line copy is a callgraph node with no
        // definition in the referencing translation unit, and the reachability gates then
        // carry no out-edge for anything a critical section reaches.
#if KICKOS_KERNEL_CORES > 1
#define KICKOS_IRQLOCK_INLINE __attribute__((always_inline))
#else
#define KICKOS_IRQLOCK_INLINE
#endif
        KICKOS_IRQLOCK_INLINE IrqLock()
            : state_(arch_irq_save())
        {
            klock_enter();
        }
        KICKOS_IRQLOCK_INLINE ~IrqLock()
        {
            klock_leave();
            arch_irq_restore(state_);
        }
#undef KICKOS_IRQLOCK_INLINE

        IrqLock(IrqLock const&) = delete;
        IrqLock& operator=(IrqLock const&) = delete;

    private:
        arch_irq_state_t state_;
    };
}

#endif
