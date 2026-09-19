// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ARMv6-M critical section, inline so IrqLock carries no call. PRIMASK is the whole mask
// on this core: the restore writes the saved word back, so a nested lock stays masked.

#ifndef KICKOS_ARCH_IRQ_INLINE_H
#define KICKOS_ARCH_IRQ_INLINE_H

// A host unit fixture can carry this arch's include directory (tests/unit/mapfence) while
// answering the seam with a definition of its own, so the bodies appear only for a real
// compile of this ISA; anywhere else the seam stays the out-of-line declaration and the
// fixture's definition is the one that links. The markers precede the arch.h include
// because arch.h declares the halves they do not claim, and a declaration ahead of these
// static bodies does not compile.
#if defined(__arm__)
#define KICKOS_ARCH_IRQ_SAVE_INLINE 1
#define KICKOS_ARCH_IRQ_RESTORE_INLINE 1
#endif

#include <kickos/arch/arch.h> // arch_irq_state_t

#if defined(__arm__)

#include <stdint.h>

static inline __attribute__((always_inline)) arch_irq_state_t arch_irq_save(void)
{
    uint32_t prev;
    __asm volatile("mrs %0, primask" : "=r"(prev));
    __asm volatile("cpsid i" ::: "memory");
    return prev;
}

static inline __attribute__((always_inline)) void arch_irq_restore(arch_irq_state_t state)
{
    __asm volatile("msr primask, %0" ::"r"(state) : "memory");
}

#endif

#endif
