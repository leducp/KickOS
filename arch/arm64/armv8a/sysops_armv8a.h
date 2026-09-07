// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The system operations the stage-1 map editor spends: the two identification registers it
// reads back, TTBR0_EL1, the barriers and the TLB maintenance.
//
// Each maintenance operation is named with its shareability, because CRm and not the mnemonic
// carries it and tests/static/check_tlbi_shareability.sh decodes CRm out of the linked image:
// a caller picks the form its posture owes and this header composes neither.

#ifndef KICKOS_ARCH_ARM64_ARMV8A_SYSOPS_ARMV8A_H
#define KICKOS_ARCH_ARM64_ARMV8A_SYSOPS_ARMV8A_H

#include <stdint.h>

extern "C"
{

#if defined(__aarch64__)

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

    // VAAE1 and not VAE1: nothing here tags a translation, so an entry must be dropped whatever
    // ASID it was cached under. The argument is the address SHIFTED by the granule.
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

#endif
}

#endif
