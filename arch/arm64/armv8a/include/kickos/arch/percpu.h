// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// AArch64: the state a vector slot must reach without a TCB, one block per core.
//
// TPIDR_EL1 is what a multi-core arm reads this block's address out of, and it is free for
// that because T6a stopped ENTER_FROM_EL0 spending it as scratch (docs/design-m6-mmu.md
// section 2). Nothing seats it at one core, where the block is a link-time address.

#ifndef KICKOS_ARCH_PERCPU_H
#define KICKOS_ARCH_PERCPU_H

#include <kickos/arch/arch.h>

#include <stdint.h>

// C++ ONLY, as arch.h itself is: the extern "C" below is UNGUARDED, so a C includer breaks
// here. Nothing but this arch's own C++ backend includes it, vectors.S reaching the block by
// symbol name and displacement.
extern "C"
{

struct armv8a_percpu
{
    // The incoming thread's kernel block top. vectors.S spells this displacement as a
    // literal, so a field added AHEAD of it is a silent wrong offset.
    uintptr_t kernel_sp;
};

extern struct armv8a_percpu kickos_armv8a_percpu[KICKOS_NUM_CORES];

// At one core the block is the array's first element and the accessor is a FOLD, so the
// image carries no load and no branch to find it: the property check_cpu_id_fold.sh holds
// for arch_cpu_id, for the same reason. The multi-core arm is a DECLARATION ONLY, reading
// TPIDR_EL1 which secondary bring-up seats, so a port that raises KICKOS_NUM_CORES and
// ships no definition is a LINK error rather than a kernel that believes it is on core 0.
#if KICKOS_NUM_CORES > 1
struct armv8a_percpu* armv8a_percpu(void);
#else
#define armv8a_percpu() (&kickos_armv8a_percpu[0])
#endif

} // extern "C"

#endif
