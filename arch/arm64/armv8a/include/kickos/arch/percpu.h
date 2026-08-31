// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// AArch64: the state a vector slot must reach without a TCB, one block per core.
//
// TPIDR_EL1 is what a multi-core arm reads this block's address out of, and it is free for
// that because ENTER_FROM_EL0 (switch.S) does not spend it as scratch. Nothing seats it at
// one core, where the block is a link-time address.

#ifndef KICKOS_ARCH_PERCPU_H
#define KICKOS_ARCH_PERCPU_H

#include <kickos/arch/arch.h>

#include <stdint.h>

// C++ ONLY, as arch.h itself is: the extern "C" below is UNGUARDED, so a C includer breaks
// here. Nothing but this arch's own C++ backend includes it, vectors.S and switch.S reaching
// the block by symbol name and displacement.
extern "C"
{

// The type and the accessor may not share a name: a function shadowing a struct hides that
// struct's constructor, which is -Wshadow and an error here.
struct armv8a_percpu_block
{
    // The incoming thread's kernel block top. vectors.S spells this displacement as a
    // literal, so a field added AHEAD of it is a silent wrong offset.
    uintptr_t kernel_sp;

    // The context PHYSICALLY on the CPU, which is not arch_switch's `from`: one ISR can
    // reschedule several times, and every call after the first names a thread the scheduler
    // has merely published, whose registers are still nowhere. The IRQ exit in switch.S saves
    // through this cell and re-seats it to the incoming context.
    struct arch_context* ctx_current;

    // The deferred switch's target, consumed at the exception exit. A target alone, so the
    // last one written wins.
    struct arch_context* switch_to;
};

extern struct armv8a_percpu_block kickos_armv8a_percpu[KICKOS_NUM_CORES];

// At one core the block is the array's first element and the accessor is a FOLD, so the
// image carries no load and no branch to find it: the property check_cpu_id_fold.sh holds
// for arch_cpu_id, for the same reason. The multi-core arm is a DECLARATION ONLY, reading
// TPIDR_EL1 which secondary bring-up seats, so a port that raises KICKOS_NUM_CORES and
// ships no definition is a LINK error rather than a kernel that believes it is on core 0.
#if KICKOS_NUM_CORES > 1
struct armv8a_percpu_block* armv8a_percpu(void);
#else
#define armv8a_percpu() (&kickos_armv8a_percpu[0])
#endif

} // extern "C"

#endif
