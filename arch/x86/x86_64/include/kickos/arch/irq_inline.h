// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The x86-64 critical section, inline so IrqLock carries no call.
//
// Nesting-safe: the state is the one bit this touches and the restore sets only what its own
// save cleared. A wholesale RFLAGS write-back would clobber the arithmetic and direction flags.

#ifndef KICKOS_ARCH_IRQ_INLINE_H
#define KICKOS_ARCH_IRQ_INLINE_H

// A host unit fixture can carry this arch's include directory (tests/unit/mapfence) while
// answering the seam with a definition of its own, so the bodies must appear only for a real
// compile of this ISA; anywhere else the seam stays the out-of-line declaration and the
// fixture's definition is the one that links. __x86_64__ ALONE DOES NOT SAY THAT, being true on
// an ordinary build host as well; the freestanding test is what keeps the bodies out of a
// hosted gate. The markers precede the arch.h include because arch.h declares the halves
// they do not claim, and a declaration ahead of these static bodies does not compile.
#if defined(__x86_64__) && __STDC_HOSTED__ == 0
#define KICKOS_ARCH_IRQ_SAVE_INLINE 1
#define KICKOS_ARCH_IRQ_RESTORE_INLINE 1
#endif

#include <kickos/arch/arch.h> // arch_irq_state_t

#if defined(__x86_64__) && __STDC_HOSTED__ == 0

#include <stdint.h>

#define KICKOS_X86_64_RFLAGS_IF (1ull << 9)

static inline __attribute__((always_inline)) arch_irq_state_t arch_irq_save(void)
{
    uint64_t flags = 0;
    __asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags)::"memory");
    return static_cast<arch_irq_state_t>(flags & KICKOS_X86_64_RFLAGS_IF);
}

static inline __attribute__((always_inline)) void arch_irq_restore(arch_irq_state_t state)
{
    if (state != 0) // IF was set before the save, so the save is what masked it
    {
        __asm__ volatile("sti" ::: "memory");
    }
}

#endif

#endif
