// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MPU commit timing for ARM. Keep the counter source consistent with bench_cyccnt().
// ARMv6-M and ARMv8-M return zero, matching their other benchmark phases.
// The interval includes interrupt masking and any interrupt before it takes effect.
// Declare the recorder here to avoid including the kernel benchmark header.

#ifndef KICKOS_ARCH_ARM_COMMON_BENCH_MPU_H
#define KICKOS_ARCH_ARM_COMMON_BENCH_MPU_H

#include <stdint.h>

#if KICKOS_BENCH

extern "C" void kickos_bench_mpu_commit(uint32_t delta);

// Inline counter reads so PH_NULL accounts for their overhead.
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
