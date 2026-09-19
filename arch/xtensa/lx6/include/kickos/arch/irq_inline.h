// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The Xtensa LX6 critical section, inline so IrqLock carries no call.
//
// check_lx6_park_mask.sh reads the secondary park out of the linked image and accepts either an
// inlined RSIL to a nonzero level or a call here; inlining this body moves it to the first arm.

#ifndef KICKOS_ARCH_IRQ_INLINE_H
#define KICKOS_ARCH_IRQ_INLINE_H

// A host unit fixture can carry this arch's include directory (tests/unit/mapfence) while
// answering the seam with a definition of its own, so the bodies appear only for a real
// compile of this ISA; anywhere else the seam stays the out-of-line declaration and the
// fixture's definition is the one that links. The markers precede the arch.h include
// because arch.h declares the halves they do not claim, and a declaration ahead of these
// static bodies does not compile.
#if defined(__XTENSA__)
#define KICKOS_ARCH_IRQ_SAVE_INLINE 1
#define KICKOS_ARCH_IRQ_RESTORE_INLINE 1
#endif

#include <kickos/arch/arch.h> // arch_irq_state_t

#if defined(__XTENSA__)

#include <stdint.h>

// Masks the C-handleable levels 1-3 (the timer + all device lines); the high-level
// 4-7 / NMI zero-latency band stays unmaskable.
#define KICKOS_LX6_IRQ_LOCK_LEVEL 3

static inline __attribute__((always_inline)) arch_irq_state_t arch_irq_save(void)
{
    uint32_t ps;
    // RSIL atomically returns the old PS and raises PS.INTLEVEL. Nesting-safe: the whole
    // PS is saved/restored, so a prior raised level is preserved.
    __asm volatile("rsil %0, %1" : "=a"(ps) : "i"(KICKOS_LX6_IRQ_LOCK_LEVEL) : "memory");
    return ps;
}

static inline __attribute__((always_inline)) void arch_irq_restore(arch_irq_state_t state)
{
    __asm volatile("wsr.ps %0; rsync" ::"a"(static_cast<uint32_t>(state)) : "memory");
}

#endif

#endif
