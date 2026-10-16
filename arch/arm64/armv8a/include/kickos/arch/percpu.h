// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// AArch64: the state a vector slot must reach without a TCB, one block per core.
//
// A multi-core image reads this block's address out of TPIDR_EL1, which ENTER_FROM_EL0
// (switch.S) must not spend as scratch. At one core the block is a link-time address.

#ifndef KICKOS_ARCH_PERCPU_H
#define KICKOS_ARCH_PERCPU_H

#include <kickos/arch/arch.h>

#include <stdint.h>

// C++ only, as arch.h is: the extern "C" below is unguarded. vectors.S and switch.S reach the
// block by symbol name and displacement.
extern "C"
{

// The type and the accessor may not share a name: a function shadowing a struct hides that
// struct's constructor, which is -Wshadow and an error here.
// One whole cache line per core: no two cores may share a line of this array.
struct alignas(64) armv8a_percpu_block
{
    // The incoming thread's kernel block top. vectors.S spells this displacement as a
    // literal, so a field added AHEAD of it is a silent wrong offset.
    uintptr_t kernel_sp;

    // The context PHYSICALLY on the CPU, which is not arch_switch's `from`: one ISR can
    // reschedule several times, and every call after the first names a thread whose registers
    // are still nowhere. The IRQ exit in switch.S saves through this cell and re-seats it.
    struct arch_context* ctx_current;

    // The deferred switch's target, consumed at the exception exit. A target alone, so the
    // last one written wins.
    struct arch_context* switch_to;

    // Nonzero exactly while an IRQ dispatch runs on this core. Written by the owning core
    // alone, under the mask the exception itself applies.
    uint32_t isr_depth;

#if KICKOS_BENCH
    // PMCCNTR_EL0 at the switch window's open, read back at its close by the same core.
    uint32_t bench_sw_start;
#endif

    // Under the reserved-root form, the TTBR0_EL1 value the next eret to EL0 installs:
    // arch_aspace_activate writes it, switch.S reads it.
    uint64_t user_ttbr0;
};

extern struct armv8a_percpu_block kickos_armv8a_percpu[KICKOS_NUM_CORES];

// At one core the accessor folds to the array's first element. The multi-core arm is a
// declaration only (it reads TPIDR_EL1, seated by secondary bring-up), so a port that raises
// KICKOS_NUM_CORES and ships no definition fails to link rather than believing it is core 0.
#if KICKOS_NUM_CORES > 1
struct armv8a_percpu_block* armv8a_percpu(void);
#else
#define armv8a_percpu() (&kickos_armv8a_percpu[0])
#endif

} // extern "C"

#endif
