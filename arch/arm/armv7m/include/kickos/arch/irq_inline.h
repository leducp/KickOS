// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ARMv7-M half of the critical section that IrqLock carries inline.
//
// ONLY THE UNMASK. arch_irq_save carries the nested-lock test and the DSB+ISB pair the BASEPRI
// raise needs, and inlining that body costs more flash than the call it removes; it stays in
// arch_armv7m.cc.

#ifndef KICKOS_ARCH_IRQ_INLINE_H
#define KICKOS_ARCH_IRQ_INLINE_H

// A host unit fixture can carry this arch's include directory (tests/unit/mapfence) while
// answering the seam with a definition of its own, so the bodies appear only for a real
// compile of this ISA; anywhere else the seam stays the out-of-line declaration and the
// fixture's definition is the one that links. The markers precede the arch.h include
// because arch.h declares the halves they do not claim, and a declaration ahead of these
// static bodies does not compile.
#if defined(__arm__)
#define KICKOS_ARCH_IRQ_RESTORE_INLINE 1
#endif

#include <kickos/arch/arch.h> // arch_irq_state_t

#if defined(__arm__)

static inline __attribute__((always_inline)) void arch_irq_restore(arch_irq_state_t state)
{
    __asm volatile("msr basepri, %0" ::"r"(state) : "memory");
}

#endif

#endif
