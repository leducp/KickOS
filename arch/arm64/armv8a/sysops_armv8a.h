// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Register access, barriers and TLB maintenance for the ARM64 map editor.
// Operation names state whether maintenance is local or Inner Shareable.

#ifndef KICKOS_ARCH_ARM64_ARMV8A_SYSOPS_ARMV8A_H
#define KICKOS_ARCH_ARM64_ARMV8A_SYSOPS_ARMV8A_H

#include <stdint.h>

// Host tests provide these operations through aspace_sysops_seam.cc.
#ifndef KICKOS_ARMV8A_SYSOPS_FROM_SEAM
#define KICKOS_ARMV8A_SYSOPS_FROM_SEAM 0
#endif

extern "C"
{

#if !KICKOS_ARMV8A_SYSOPS_FROM_SEAM

    inline uint64_t kickos_armv8a_read_tcr_el1(void)
    {
        uint64_t tcr = 0;
        __asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
        return tcr;
    }

    inline uint64_t kickos_armv8a_read_mmfr0_el1(void)
    {
        uint64_t mmfr0 = 0;
        __asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
        return mmfr0;
    }

    inline uint64_t kickos_armv8a_read_ttbr0_el1(void)
    {
        uint64_t ttbr = 0;
        __asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));
        return ttbr;
    }

    inline void kickos_armv8a_write_ttbr0_el1(uint64_t ttbr)
    {
        __asm volatile("msr ttbr0_el1, %0" ::"r"(ttbr) : "memory");
    }

    inline void kickos_armv8a_dsb_ishst(void)
    {
        __asm volatile("dsb ishst" ::: "memory");
    }

    inline void kickos_armv8a_dsb_ish(void)
    {
        __asm volatile("dsb ish" ::: "memory");
    }

    inline void kickos_armv8a_isb(void)
    {
        __asm volatile("isb" ::: "memory");
    }

    // Invalidate this address under every ASID. The operand is VA / granule size.
    inline void kickos_armv8a_tlbi_page_is(uint64_t page)
    {
        __asm volatile("tlbi vaae1is, %0" ::"r"(page) : "memory");
    }

    inline void kickos_armv8a_tlbi_page_local(uint64_t page)
    {
        __asm volatile("tlbi vaae1, %0" ::"r"(page) : "memory");
    }

    inline void kickos_armv8a_tlbi_all_is(void)
    {
        __asm volatile("tlbi vmalle1is" ::: "memory");
    }

    inline void kickos_armv8a_tlbi_all_local(void)
    {
        __asm volatile("tlbi vmalle1" ::: "memory");
    }

    // PAR_EL1 after a stage 1 read translation of va, at EL0 where `user` and at EL1 otherwise.
    inline uint64_t kickos_armv8a_at_read(uint64_t va, bool user)
    {
        if (user)
        {
            __asm volatile("at s1e0r, %0" ::"r"(va) : "memory");
        }
        else
        {
            __asm volatile("at s1e1r, %0" ::"r"(va) : "memory");
        }
        __asm volatile("isb" ::: "memory");
        uint64_t par = 0;
        __asm volatile("mrs %0, par_el1" : "=r"(par));
        return par;
    }

    inline uint64_t kickos_armv8a_read_mair_el1(void)
    {
        uint64_t mair = 0;
        __asm volatile("mrs %0, mair_el1" : "=r"(mair));
        return mair;
    }

#else

    uint64_t kickos_armv8a_read_tcr_el1(void);
    uint64_t kickos_armv8a_read_mmfr0_el1(void);
    uint64_t kickos_armv8a_read_ttbr0_el1(void);
    void kickos_armv8a_write_ttbr0_el1(uint64_t ttbr);
    void kickos_armv8a_dsb_ishst(void);
    void kickos_armv8a_dsb_ish(void);
    void kickos_armv8a_isb(void);
    void kickos_armv8a_tlbi_page_is(uint64_t page);
    void kickos_armv8a_tlbi_page_local(uint64_t page);
    void kickos_armv8a_tlbi_all_is(void);
    void kickos_armv8a_tlbi_all_local(void);
    uint64_t kickos_armv8a_at_read(uint64_t va, bool user);
    uint64_t kickos_armv8a_read_mair_el1(void);

#endif
}

#endif
