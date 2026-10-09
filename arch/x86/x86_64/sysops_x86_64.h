// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Identification, control registers and TLB maintenance for the x86-64 map editor.

#ifndef KICKOS_ARCH_X86_X86_64_SYSOPS_X86_64_H
#define KICKOS_ARCH_X86_X86_64_SYSOPS_X86_64_H

#include <stdint.h>

// Host tests provide these operations through x86_64_sysops_seam.cc.
#ifndef KICKOS_X86_64_SYSOPS_FROM_SEAM
#define KICKOS_X86_64_SYSOPS_FROM_SEAM 0
#endif

#if !KICKOS_X86_64_SYSOPS_FROM_SEAM
#include <kickos/arch/regs.h>
#endif

extern "C"
{

#if !KICKOS_X86_64_SYSOPS_FROM_SEAM

    inline void kickos_x86_64_cpuid(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b,
                                    uint32_t* c, uint32_t* d)
    {
        __asm__ volatile("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(sub));
    }

    // This core only.
    inline void kickos_x86_64_invlpg(uintptr_t va)
    {
        __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    }

    inline uint64_t kickos_x86_64_read_cr0(void)
    {
        return kickos::x86_64::read_cr0();
    }

    inline void kickos_x86_64_write_cr0(uint64_t v)
    {
        kickos::x86_64::write_cr0(v);
    }

    inline uint64_t kickos_x86_64_read_cr3(void)
    {
        return kickos::x86_64::read_cr3();
    }

    inline void kickos_x86_64_write_cr3(uint64_t v)
    {
        kickos::x86_64::write_cr3(v);
    }

    inline uint64_t kickos_x86_64_read_cr4(void)
    {
        return kickos::x86_64::read_cr4();
    }

    inline uint64_t kickos_x86_64_read_msr(uint32_t index)
    {
        return kickos::x86_64::read_msr(index);
    }

    inline void kickos_x86_64_write_msr(uint32_t index, uint64_t value)
    {
        kickos::x86_64::write_msr(index, value);
    }

#else

    void kickos_x86_64_cpuid(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c,
                             uint32_t* d);
    void kickos_x86_64_invlpg(uintptr_t va);
    uint64_t kickos_x86_64_read_cr0(void);
    void kickos_x86_64_write_cr0(uint64_t v);
    uint64_t kickos_x86_64_read_cr3(void);
    void kickos_x86_64_write_cr3(uint64_t v);
    uint64_t kickos_x86_64_read_cr4(void);
    uint64_t kickos_x86_64_read_msr(uint32_t index);
    void kickos_x86_64_write_msr(uint32_t index, uint64_t value);

#endif
}

#endif
