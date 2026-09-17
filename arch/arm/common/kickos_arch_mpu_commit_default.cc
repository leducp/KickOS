// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// Commit the deferred stash to the PMSAv7 hardware (F411/XMC on v7-M, RP2040/microbit
// on v6-M). cpsid brackets the disable/reprogram/re-enable so a preempting IRQ cannot
// observe a half-programmed MPU; valid asm on both v6-M and v7-M. A chip with a
// different MPU (K64F SYSMPU) or a v8-M core (arch_arm_pmsav8.cc) defines its own and
// reads the SAME stash through kickos_arm_mpu_pending.
//
// Built at KICKOS_HAVE_MPU=0 too: the reference is unconditional, so dropping this TU
// there leaves the symbol undefined.

#include <kickos/arch/arch.h>

#include "bench_mpu.h"

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU

extern "C"
{
    struct arch_mpu_encoded const* kickos_arm_mpu_pending(void);
    void kickos_arm_mpu_program(struct arch_mpu_encoded const* img);

    void kickos_arch_mpu_commit(void)
    {
#if KICKOS_BENCH
        uint32_t const bench_start = kickos_arm_mpu_bench_cyc();
#endif
        struct arch_mpu_encoded const* const img = kickos_arm_mpu_pending();
        // An empty stash writes no descriptor, and a sample of the masking pair alone would
        // sit under every real one as this row's min. kickos_arm_mpu_program already
        // returns on a null image, so the two paths run the same hardware either way.
        if (img == nullptr)
        {
            return;
        }
        uint32_t primask;
        __asm volatile("mrs %0, primask" : "=r"(primask));
        __asm volatile("cpsid i" ::: "memory");
        kickos_arm_mpu_program(img);
        __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
#if KICKOS_BENCH
        kickos_bench_mpu_commit(kickos_arm_mpu_bench_cyc() - bench_start);
#endif
    }
}

#else

extern "C" void kickos_arch_mpu_commit(void)
{
}

#endif
