// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ARMv8-A critical section, inline so IrqLock carries no call.
//
// NESTING-SAFE: the state is the one bit this touches and the restore clears only what its own
// save set. A wholesale `msr daif, saved` would write back D, A and F too, clobbering any change
// made between the two.

#ifndef KICKOS_ARCH_IRQ_INLINE_H
#define KICKOS_ARCH_IRQ_INLINE_H

// A host unit fixture can carry this include directory (tests/unit/mapfence) and define the
// seam itself, so the bodies appear only in a freestanding compile: __aarch64__ alone is true
// on an aarch64 build host too. The markers precede the arch.h include because a declaration
// ahead of these static bodies does not compile.
#if defined(__aarch64__) && __STDC_HOSTED__ == 0
#define KICKOS_ARCH_IRQ_SAVE_INLINE 1
#define KICKOS_ARCH_IRQ_RESTORE_INLINE 1
#endif

#include <kickos/arch/arch.h> // arch_irq_state_t

#if defined(__aarch64__) && __STDC_HOSTED__ == 0

#include <stdint.h>

#define KICKOS_ARMV8A_DAIF_I (1ULL << 7) // PSTATE.I within DAIF

static inline __attribute__((always_inline)) arch_irq_state_t arch_irq_save(void)
{
    uint64_t daif = 0;
    __asm volatile("mrs %0, daif" : "=r"(daif));
    __asm volatile("msr daifset, #2" ::: "memory"); // DAIFSet bit 1 == I
    return static_cast<arch_irq_state_t>(daif & KICKOS_ARMV8A_DAIF_I);
}

static inline __attribute__((always_inline)) void arch_irq_restore(arch_irq_state_t state)
{
    if (state == 0) // I was clear before the save, so the save is what masked it
    {
        __asm volatile("msr daifclr, #2" ::: "memory");
    }
}

#endif

#endif
