// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// PMSAv8 MPU backend for Cortex-M33. Overrides the shared ARM commit and
// region-encoding functions. PMSAv8 uses RBAR/RLAR with 32-byte-aligned
// base/limit ranges; PMSAv7 RASR encoding is incompatible.
// arch_mpu_apply stores the incoming set. PendSV commits it after switching.
// Selected through KICKOS_ARM_PMSAV8_SOURCE, from the chip file's pmsav8 unit.

#include <kickos/arch/arch.h>

#include "bench_mpu.h"
#include "mpu.h"
#include "pmsav8_encode.h"
#include "regs_v8m.h"

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU

using namespace kickos::arm;

extern "C"
{

// Read the shared pending stash written by arch_mpu_apply (arch_arm_common.cc).
struct arch_mpu_encoded const* kickos_arm_mpu_pending(void);

// The descriptor words this core's MPU holds, and whether that record may be believed.
// A commit programs only the slots whose words differ from it, so anything that writes a
// descriptor outside the loop below must invalidate it first: a partial commit leaves a
// disagreement standing where a total one erased it. The MPU is per-core banked, hence one
// record per core rather than one per image.
//
// Not the pending image POINTER: MpuSet re-encodes in place and thread slots come from a
// static pool, so a pointer that was programmed once can address different words later.
static struct arch_mpu_encoded g_mpu_held[KICKOS_NUM_CORES];
static bool g_mpu_held_valid[KICKOS_NUM_CORES];

// One-time PMSAv8 setup: the MAIR attribute indirection + MemManage enable. Must run
// BEFORE the scheduler starts. This is also the LINK ANCHOR: chip_rp2350.o, always
// pulled for arch_init, references this symbol, which is defined ONLY here, so GNU ld
// pulls this member and resolves kickos_arch_mpu_commit / arch_mpu_region_encodable to
// the overrides below. Without that reference the fallback TU answers them first and
// the board silently declines.
//
// The MPU is per-core banked, so this must run once PER CORE at bring-up.
void kickos_arm_pmsav8_init(void)
{
    // kickos_arch_mpu_commit zeroes MPU_CTRL and reprograms per-thread rows ONLY, so a chip
    // fixed row would be dropped on every switch and never rewritten. Refuse such a chip at
    // boot rather than dropping it silently, spinning as kickos_arm_mpu_fixed_init does:
    // the arch path has no kernel assert to raise.
    struct kickos_arm_mpu_fixed_region const* fixed = nullptr;
    if (kickos_arm_mpu_fixed(&fixed) != 0)
    {
        while (true)
        {
            __asm volatile("wfi");
        }
    }

    // The per-thread rows the commit programs must EXIST in this part, or the slots above
    // DREGION would be dropped and the grants in them silently lost. Same refusal and the
    // same reason as PMSAv7's (arch_arm_mpu_pmsav7.cc): a part too narrow to hold a full
    // per-thread set is refused, never truncated. No fixed count to add, the refusal above
    // having just excluded one.
    size_t const hw_regions = (reg32(MPU_TYPE) >> 8) & 0xFFu;
    if (ARCH_MPU_ENCODED_SLOTS > hw_regions)
    {
        while (true)
        {
            __asm volatile("wfi");
        }
    }

    // This core's descriptors stand at their reset values here, and the MAIR indirection
    // below changes what an already-programmed AttrIndx would have meant.
    g_mpu_held_valid[arch_cpu_id()] = false;

    // slot0 Normal cacheable, slot1 Device, slot2 Normal non-cacheable
    reg32(MPU_MAIR0) =
        MAIR_NORMAL_WBWA | (MAIR_DEVICE_nGnRE << 8) | (MAIR_NORMAL_NC << 16);
    reg32(MPU_MAIR1) = 0;
    reg32(SCB_SHCSR) |= SHCSR_MEMFAULTENA; // MPU violation -> MemManage, not escalated HardFault
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    // MPU left DISABLED here; the first context switch's commit enables it with
    // PRIVDEFENA. Until then the privileged boot runs on the default memory map.
}

uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                         struct arch_mpu_encoded* out)
{
    return pmsav8_encode(regions, n, out);
}

// Replaces the PMSAv7 kickos_arch_mpu_commit fallback. Programs the running thread's
// per-thread regions from the shared stash into RBAR/RLAR, disabling the unused
// descriptors up to MPU_TYPE.DREGION. Runs AFTER the physical swap. cpsid brackets the
// disable/reprogram/re-enable: PendSV is lowest priority, so a device IRQ could
// otherwise preempt a half-programmed MPU, the caller's BASEPRI IrqLock having lapsed
// by the time the deferred commit runs. Every descriptor on the M33 is a per-thread
// grant, which is what makes the MPU_CTRL zeroing below sound.
void kickos_arch_mpu_commit(void)
{
#if KICKOS_BENCH
    uint32_t const bench_start = kickos_arm_mpu_bench_cyc();
#endif
    struct arch_mpu_encoded const* const img = kickos_arm_mpu_pending();
    if (img == nullptr)
    {
        return;
    }
    uint32_t const core = arch_cpu_id();
    struct arch_mpu_encoded* const held = &g_mpu_held[core];
    bool const total = not g_mpu_held_valid[core];
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("cpsid i" ::: "memory");

    // Zeroing MPU_CTRL also suspends any chip fixed row for the whole reprogram window, and
    // the loop below rewrites per-thread rows only. Sound because every row on this backend
    // is per-thread, which kickos_arm_pmsav8_init enforces. PMSAv7 must NOT do this
    // (imxrt1062's anti-speculation wrap).
    //
    // The disable/re-enable pair is per COMMIT and cannot be skipped the way a descriptor can:
    // this revision has no way to move one row with the MPU live. Only the loop shortens.
    reg32(MPU_CTRL) = 0; // disable while reprogramming (a switch must take effect atomically)
    __asm volatile("dsb" ::: "memory");

    // DREGION is silicon config (the M33 on RP2350 implements 8); read it, never hard-code.
    size_t const hw_regions = (reg32(MPU_TYPE) >> 8) & 0xFFu;
    for (size_t i = 0; i < hw_regions; i++)
    {
        if (i >= ARCH_MPU_ENCODED_SLOTS)
        {
            // Past the image: disabled by the total commit and never written again, no
            // image reaching this far.
            if (not total)
            {
                continue;
            }
            reg32(MPU_RNR) = static_cast<uint32_t>(i);
            reg32(MPU_RLAR) = 0; // EN=0: disable the descriptor
            continue;
        }
        if (not total and held->rbar[i] == img->rbar[i] and held->rlar[i] == img->rlar[i])
        {
            continue;
        }
        reg32(MPU_RNR) = static_cast<uint32_t>(i);
        reg32(MPU_RBAR) = img->rbar[i];
        reg32(MPU_RLAR) = img->rlar[i];
        held->rbar[i] = img->rbar[i];
        held->rlar[i] = img->rlar[i];
    }
    g_mpu_held_valid[core] = true;

    __asm volatile("dsb" ::: "memory");
    reg32(MPU_CTRL) = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA; // priv uses default map
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
#if KICKOS_BENCH
    kickos_bench_mpu_commit(kickos_arm_mpu_bench_cyc() - bench_start);
#endif
}

// PMSAv8 is byte-granular on a 32-byte page: a window is encodable EXACTLY iff base and
// base+size both land on a 32-byte boundary, any multiple of the granule being nameable.
// arch_mpu_min_region keeps the shared 32-byte answer.
bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size < 32u)
    {
        return false;
    }
    return (base & 31u) == 0 and (size & 31u) == 0;
}

// Replaces the v7-M fallback 1. Reached only in an enforcement build, this TU sitting
// inside KICKOS_HAVE_MPU.
int arch_mpu_region_pow2(void)
{
    return 0;
}

}

#endif // KICKOS_HAVE_MPU
