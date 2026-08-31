// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv8-M (PMSAv8) MPU backend for Cortex-M33 chips (RP2350, and any future
// nRF5340 / STM32U5 / STM32H5 M33 part). The M33 shares the whole armv7m arch
// (BASEPRI crit, DWT, SysTick, NVIC, PendSV switch + SVC trampoline) and differs only
// in the MPU, so this file is a STRONG override of exactly two shared arch-seam
// symbols:
//
//   kickos_arch_mpu_commit: programs the running thread's regions into the PMSAv8
//     RBAR/RLAR pair. The v7-M path writes RASR values to what is RLAR on v8-M, and
//     clears the RBAR low bits that are now SH/AP/XN -> AP=priv-only, so an
//     unprivileged thread faults on its own stack.
//   arch_mpu_region_encodable: 32-byte-granular, PMSAv8 taking an arbitrary
//     32-byte-aligned [base, base+size).
//
// The stash-only arch_mpu_apply (arch_arm_common.cc) is SHARED unchanged: it records
// the incoming region set, and the armv7m PendSV epilogue calls
// kickos_arch_mpu_commit AFTER the physical swap (the deferred-commit seam), which
// lands here.
//
// This TU enters the CHIP library only for a PMSAv8 chip (arch/arm/chip/<chip>/mpu.cmake
// sets KICKOS_ARM_PMSAV8_SOURCE), so a PMSA board links the shared commit fallback
// instead.

#include <kickos/arch/arch.h>

#include "mpu.h"
#include "regs_v8m.h"

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU

namespace
{
    using namespace kickos::arm;

    // {base,size,attr} -> the MPU_RBAR low attribute bits (SH|AP|XN). attr is the
    // UNPRIVILEGED access; supervisor comes from the PRIVDEFENA background. Code is
    // RO+executable (Normal), data/stack RW+execute-never, and a read-only data region
    // RO-any.
    uint32_t pmsav8_rbar_attr(uint32_t attr)
    {
        if (attr & ARCH_MPU_X)
        {
            return RBAR_AP_RO_ANY; // code: RO-any, executable (XN=0), SH=0
        }
        uint32_t v = RBAR_XN; // data / MMIO: execute-never
        if (attr & ARCH_MPU_W)
        {
            v |= RBAR_AP_RW_ANY;
        }
        else
        {
            v |= RBAR_AP_RO_ANY;
        }
        return v;
    }

    // {attr} -> the MPU_RLAR AttrIndx bits, i.e. the MAIR0 slot programmed below.
    uint32_t pmsav8_rlar_attr(uint32_t attr)
    {
        if (attr & ARCH_MPU_DEV)
        {
            return RLAR_ATTR_DEVICE;
        }
        if (attr & ARCH_MPU_NOCACHE)
        {
            return RLAR_ATTR_NORMAL_NC;
        }
        return RLAR_ATTR_NORMAL;
    }
}

extern "C"
{

// Read the shared pending stash written by arch_mpu_apply (arch_arm_common.cc).
struct arch_mpu_encoded const* kickos_arm_mpu_pending(void);

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

// Pack the region set into the RBAR/RLAR pair per slot. RBAR masks the base to a 32-byte
// boundary and RLAR the limit, so a region PMSAv8 cannot name exactly gets RLAR 0 (EN=0)
// rather than a window rounded outward from what was asked.
uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                         struct arch_mpu_encoded* out)
{
    if (n > ARCH_MPU_ENCODED_SLOTS)
    {
        n = ARCH_MPU_ENCODED_SLOTS;
    }
    uint32_t seated = 0;
    size_t i = 0;
    for (; i < n; i++)
    {
        out->rbar[i] = 0;
        out->rlar[i] = 0;
        if (arch_mpu_region_encodable(regions[i].base, regions[i].size))
        {
            uintptr_t const base = regions[i].base;
            uintptr_t const limit = base + regions[i].size - 1; // inclusive top
            out->rbar[i] = (static_cast<uint32_t>(base) & RBAR_BASE_MASK)
                | pmsav8_rbar_attr(regions[i].attr);
            out->rlar[i] = (static_cast<uint32_t>(limit) & RLAR_LIMIT_MASK)
                | pmsav8_rlar_attr(regions[i].attr) | RLAR_EN;
            seated |= static_cast<uint32_t>(1) << i;
        }
    }
    for (; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        out->rbar[i] = 0;
        out->rlar[i] = 0;
    }
    return seated;
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
    struct arch_mpu_encoded const* const img = kickos_arm_mpu_pending();
    if (img == nullptr)
    {
        return;
    }

    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("cpsid i" ::: "memory");

    // Zeroing MPU_CTRL also suspends any chip fixed row for the whole reprogram window, and
    // the loop below rewrites per-thread rows only. Sound because every row on this backend
    // is per-thread, which kickos_arm_pmsav8_init enforces. PMSAv7 must NOT do this
    // (imxrt1062's anti-speculation wrap).
    reg32(MPU_CTRL) = 0; // disable while reprogramming (a switch must take effect atomically)
    __asm volatile("dsb" ::: "memory");

    // DREGION is silicon config (the M33 on RP2350 implements 8); read it, never hard-code.
    size_t const hw_regions = (reg32(MPU_TYPE) >> 8) & 0xFFu;
    for (size_t i = 0; i < hw_regions; i++)
    {
        reg32(MPU_RNR) = static_cast<uint32_t>(i);
        if (i < ARCH_MPU_ENCODED_SLOTS)
        {
            reg32(MPU_RBAR) = img->rbar[i];
            reg32(MPU_RLAR) = img->rlar[i];
        }
        else
        {
            reg32(MPU_RLAR) = 0; // EN=0: disable the descriptor
        }
    }

    __asm volatile("dsb" ::: "memory");
    reg32(MPU_CTRL) = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA; // priv uses default map
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
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
