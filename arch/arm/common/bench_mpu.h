// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bench bracket every ARM deferred MPU commit carries. Declared rather than included:
// these translation units are below <kickos/bench.h>, as arch/riscv/rv32imac is.
//
// THE COUNTER ARM MUST TRACK bench_cyccnt() in <kickos/bench.h>. A board reading a source
// the rest of the phase table does not would put PH_MPU_COMMIT on another clock, and
// nothing in the table would say so. armv6m has no DWT and armv8-M is not a bench_cyccnt
// arm either, so both read zero here exactly as they do for every other phase.
//
// The window opens BEFORE the commit masks interrupts, so a device interrupt taken in the
// prologue lands in this row's max.

#ifndef KICKOS_ARCH_ARM_COMMON_BENCH_MPU_H
#define KICKOS_ARCH_ARM_COMMON_BENCH_MPU_H

#include <stdint.h>

#if KICKOS_BENCH

extern "C" void kickos_bench_mpu_commit(uint32_t delta);

// always_inline: an out-of-line copy charges two call/ret pairs to a delta PH_NULL prices
// as two bare counter reads.
static __attribute__((always_inline)) inline uint32_t kickos_arm_mpu_bench_cyc(void)
{
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
    return *reinterpret_cast<volatile uint32_t*>(0xE0001004u); // DWT CYCCNT
#else
    return 0;
#endif
}

#endif

#endif
