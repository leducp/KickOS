// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RV32 critical section, inline so IrqLock carries no call.

#ifndef KICKOS_ARCH_IRQ_INLINE_H
#define KICKOS_ARCH_IRQ_INLINE_H

// A host unit fixture can carry this arch's include directory (tests/unit/mapfence) while
// answering the seam with a definition of its own, so the bodies appear only for a real
// compile of this ISA; anywhere else the seam stays the out-of-line declaration and the
// fixture's definition is the one that links. The markers precede the arch.h include
// because arch.h declares the halves they do not claim, and a declaration ahead of these
// static bodies does not compile.
#if defined(__riscv)
#define KICKOS_ARCH_IRQ_SAVE_INLINE 1
#define KICKOS_ARCH_IRQ_RESTORE_INLINE 1
#endif

#include <kickos/arch/arch.h> // arch_irq_state_t

#if defined(__riscv)

#include <stdint.h>

#define KICKOS_RV32_MSTATUS_MIE 0x8u

static inline __attribute__((always_inline)) arch_irq_state_t arch_irq_save(void)
{
    uint32_t old;
    __asm volatile("csrrci %0, mstatus, 0x8" : "=r"(old)::"memory");
    return old & KICKOS_RV32_MSTATUS_MIE;
}

static inline __attribute__((always_inline)) void arch_irq_restore(arch_irq_state_t state)
{
    // csrs only SETS bits, and state is 0 or MSTATUS_MIE, so this re-enables MIE exactly
    // when the paired save disabled it. That is what makes it nesting-safe.
    __asm volatile("csrs mstatus, %0" ::"r"(state) : "memory");
}

#endif

#endif
