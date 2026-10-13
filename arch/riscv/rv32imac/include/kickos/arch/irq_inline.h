// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RV32 critical section, inline so IrqLock carries no call.

#ifndef KICKOS_ARCH_IRQ_INLINE_H
#define KICKOS_ARCH_IRQ_INLINE_H

// Defined only for a real RISC-V compile: a host fixture carrying this include directory
// supplies its own seam. The markers must precede the arch.h include, which declares the
// halves they do not claim.
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
    // csrs only sets bits and state is 0 or MIE, so a nested restore never enables early.
    __asm volatile("csrs mstatus, %0" ::"r"(state) : "memory");
}

#endif

#endif
