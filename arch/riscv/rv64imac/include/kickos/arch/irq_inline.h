// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RV64 critical section, inline so IrqLock carries no call.

#ifndef KICKOS_ARCH_IRQ_INLINE_H
#define KICKOS_ARCH_IRQ_INLINE_H

// Host unit fixtures (tests/unit/mapfence) carry this include directory and define the seam
// themselves, so the inline bodies exist only under a RISC-V compile. The markers precede the
// arch.h include: a declaration ahead of a static body does not compile.
#if defined(__riscv)
#define KICKOS_ARCH_IRQ_SAVE_INLINE 1
#define KICKOS_ARCH_IRQ_RESTORE_INLINE 1
#endif

#include <kickos/arch/arch.h> // arch_irq_state_t

#if defined(__riscv)

#include <stdint.h>

#include <kickos/arch/rv64_frame.h>

static inline __attribute__((always_inline)) arch_irq_state_t arch_irq_save(void)
{
    uint64_t old = 0;
    __asm volatile("csrrci %0, sstatus, 2" : "=r"(old)::"memory");
    return static_cast<arch_irq_state_t>(old & KICKOS_RV64_SSTATUS_SIE);
}

static inline __attribute__((always_inline)) void arch_irq_restore(arch_irq_state_t state)
{
    // csrs only sets bits and state is 0 or SSTATUS_SIE: SIE is re-enabled only if the paired
    // save cleared it.
    __asm volatile("csrs sstatus, %0" ::"r"(static_cast<uint64_t>(state)) : "memory");
}

#endif

#endif
