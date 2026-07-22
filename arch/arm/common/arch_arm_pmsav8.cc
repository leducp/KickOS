// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv8-M (PMSAv8) MPU backend for Cortex-M33 chips (RP2350, and any future
// nRF5340 / STM32U5 / STM32H5 M33 part). The M33 reuses the armv7m arch for
// everything else (BASEPRI crit, DWT, SysTick, NVIC, PendSV switch + SVC
// trampoline); ONLY the MPU differs. So this is NOT a whole new arch: it is a
// STRONG override of the two shared arch-seam symbols that the v7-M PMSA backend
// gets wrong on a v8-M core:
//
//   kickos_arch_mpu_commit  -- programs the running thread's regions into the
//                              PMSAv8 RBAR/RLAR pair (the v7-M path writes RASR
//                              values to what is RLAR on v8-M, and clears the RBAR
//                              low bits that are now SH/AP/XN -> AP=priv-only, so an
//                              unprivileged thread faults on its own stack).
//   arch_mpu_region_encodable -- 32-byte-granular (PMSAv8 takes an arbitrary
//                              32-byte-aligned [base, base+size); no power-of-two).
//
// The stash-only arch_mpu_apply (arch_arm_common.cc) is SHARED unchanged: it records
// the incoming region set; the armv7m PendSV epilogue calls kickos_arch_mpu_commit
// AFTER the physical swap (the deferred-commit seam), which lands here. This is the
// K64F/SYSMPU precedent exactly -- a chip with a non-PMSAv7 MPU overrides only the
// commit and reads the same stash via kickos_arm_mpu_pending.
//
// This TU is NOT in the always-compiled kickos_arch_armv7m source list; it is pulled
// into the CHIP library only for a PMSAv8 chip (arch/arm/chip/<chip>/mpu.cmake sets
// KICKOS_ARM_PMSAV8_SOURCE). The v6-M/v7-M PMSA fleet never links it, so their weak
// commit stands byte-for-byte -- isolation is at link granularity, stronger than an
// #ifdef fork inside one TU. See docs/design-rp2350-mpu-armv8m.md.

#include <kickos/arch/arch.h>

#include "regs_v8m.h"

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU

namespace
{
    using namespace kickos::arm;

    // {base,size,attr} -> the MPU_RBAR low attribute bits (SH|AP|XN). attr is the
    // UNPRIVILEGED access; supervisor comes from the PRIVDEFENA background. Keys off
    // X/DEV/W exactly like the v7-M mpu_rasr does. Code is RO+executable (Normal);
    // data/stack is RW+execute-never; a read-only data region (W absent) is RO-any.
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

    // {attr} -> the MPU_RLAR AttrIndx bits: MMIO selects the Device MAIR slot, all
    // other memory (code, data, stack) selects the Normal cacheable slot.
    uint32_t pmsav8_rlar_attr(uint32_t attr)
    {
        if (attr & ARCH_MPU_DEV)
        {
            return RLAR_ATTR_DEVICE;
        }
        return RLAR_ATTR_NORMAL;
    }
}

extern "C"
{

// Read the shared pending stash written by the weak arch_mpu_apply (arch_arm_common.cc).
size_t kickos_arm_mpu_pending(struct arch_mpu_region const** out);

// One-time PMSAv8 setup: the MAIR attribute indirection + MemManage enable. Called
// from the chip arch_init (chip_rp2350.cc) BEFORE the scheduler starts. This is also
// the LINK ANCHOR: chip_rp2350.o (always pulled for arch_init) references this symbol,
// which is defined ONLY here -- so GNU ld pulls this member, and its strong
// kickos_arch_mpu_commit / arch_mpu_region_encodable then override the weak v7-M defs.
// (A strong override in an un-referenced archive member would NOT win; the weak def in
// the already-linked arch_arm_common.o would satisfy the symbol and this member would
// never be pulled. The arch_init reference is what forces it in -- no -Wl,-u needed.)
//
// SMP (M5): the MPU is per-core banked; this runs once PER CORE (folded into per-core
// bring-up), not once globally -- correct as long as each core calls it at bring-up.
void kickos_arm_pmsav8_init(void)
{
    reg32(MPU_MAIR0) = MAIR_NORMAL_WBWA | (MAIR_DEVICE_nGnRE << 8); // slot0 Normal, slot1 Device
    reg32(MPU_MAIR1) = 0;
    reg32(SCB_SHCSR) |= SHCSR_MEMFAULTENA; // MPU violation -> MemManage, not escalated HardFault
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    // MPU left DISABLED here; the first context switch's commit enables it with
    // PRIVDEFENA. Until then the privileged boot runs on the default memory map.
}

// STRONG override of the weak PMSAv7 kickos_arch_mpu_commit. Programs the running
// thread's per-thread regions from the shared stash into RBAR/RLAR, disabling the
// unused descriptors up to MPU_TYPE.DREGION. Called from the armv7m PendSV epilogue
// AFTER the physical swap. cpsid brackets the disable/reprogram/re-enable: PendSV is
// lowest priority, so a device IRQ could otherwise preempt a half-programmed MPU (the
// caller's BASEPRI IrqLock no longer holds by the time the deferred commit runs).
// There are NO chip fixed regions on the M33 (unlike the imxrt1062 anti-speculation
// wrap): the Cortex-M33 is not speculative in the way the M7 is, so every descriptor
// is a per-thread grant.
void kickos_arch_mpu_commit(void)
{
    struct arch_mpu_region const* regions;
    size_t const n = kickos_arm_mpu_pending(&regions);

    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("cpsid i" ::: "memory");

    reg32(MPU_CTRL) = 0; // disable while reprogramming (a switch must take effect atomically)
    __asm volatile("dsb" ::: "memory");

    // DREGION is silicon config (the M33 on RP2350 implements 8); read it, never hard-code.
    size_t const hw_regions = (reg32(MPU_TYPE) >> 8) & 0xFFu;
    for (size_t i = 0; i < hw_regions; i++)
    {
        reg32(MPU_RNR) = static_cast<uint32_t>(i);
        if (i < n and regions[i].size >= 32)
        {
            uintptr_t const base = regions[i].base;
            uintptr_t const limit = base + regions[i].size - 1; // inclusive top
            reg32(MPU_RBAR) =
                (static_cast<uint32_t>(base) & RBAR_BASE_MASK) | pmsav8_rbar_attr(regions[i].attr);
            reg32(MPU_RLAR) = (static_cast<uint32_t>(limit) & RLAR_LIMIT_MASK)
                | pmsav8_rlar_attr(regions[i].attr) | RLAR_EN;
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

// PMSAv8 is byte-granular on a 32-byte page: a window is encodable EXACTLY iff base
// and base+size both land on a 32-byte boundary (no power-of-two rule, unlike the weak
// v7-M PMSA). Overrides the weak arch_mpu_region_encodable. arch_mpu_min_region stays
// the weak 32 (PMSAv8 granule == v7-M granule), so no override for it.
bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size < 32u)
    {
        return false;
    }
    return (base & 31u) == 0 and (size & 31u) == 0;
}

}

#endif // KICKOS_HAVE_MPU
