// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// x86_64: the control registers and the model-specific registers.

#ifndef KICKOS_ARCH_REGS_H
#define KICKOS_ARCH_REGS_H

#include <stdint.h>

namespace kickos::x86_64
{
    // volatile keeps a read where it stands and keeps a second one from folding into the first.
    static inline uint64_t read_cr0(void)
    {
        uint64_t v = 0;
        __asm__ volatile("movq %%cr0, %0" : "=r"(v));
        return v;
    }

    // The linear address of the last page fault, and stale contents after anything else.
    static inline uint64_t read_cr2(void)
    {
        uint64_t v = 0;
        __asm__ volatile("movq %%cr2, %0" : "=r"(v));
        return v;
    }

    static inline uint64_t read_cr3(void)
    {
        uint64_t v = 0;
        __asm__ volatile("movq %%cr3, %0" : "=r"(v));
        return v;
    }

    static inline uint64_t read_cr4(void)
    {
        uint64_t v = 0;
        __asm__ volatile("movq %%cr4, %0" : "=r"(v));
        return v;
    }

    // The memory clobber is load-bearing: CR0.WP, CR0.PG, the CR3 root and the CR4 paging bits
    // each change how the machine translates or protects an access, so nothing may be moved
    // across one. A CR3 reload is also this port's translation-cache flush.
    static inline void write_cr0(uint64_t v)
    {
        __asm__ volatile("movq %0, %%cr0" ::"r"(v) : "memory");
    }

    static inline void write_cr3(uint64_t v)
    {
        __asm__ volatile("movq %0, %%cr3" ::"r"(v) : "memory");
    }

    static inline void write_cr4(uint64_t v)
    {
        __asm__ volatile("movq %0, %%cr4" ::"r"(v) : "memory");
    }

    static inline uint64_t read_msr(uint32_t index)
    {
        uint32_t lo = 0;
        uint32_t hi = 0;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(index));
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    // The memory clobber is load-bearing: every register this port writes changes state a later
    // access depends on. The caller still owes the translation-cache maintenance an IA32_PAT or
    // an EFER.NXE write requires.
    static inline void write_msr(uint32_t index, uint64_t value)
    {
        uint32_t const lo = static_cast<uint32_t>(value);
        uint32_t const hi = static_cast<uint32_t>(value >> 32);
        __asm__ volatile("wrmsr" ::"a"(lo), "d"(hi), "c"(index) : "memory");
    }

    // The handover capture, arch/x86/x86_64/entry_x86_64.cc, which reads the state firmware
    // left before the first `cli` and the first console write.
    static inline uint64_t read_cr0_ordered(void)
    {
        uint64_t v = 0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(v) : : "memory");
        return v;
    }

    static inline uint64_t read_cr3_ordered(void)
    {
        uint64_t v = 0;
        __asm__ volatile("mov %%cr3, %0" : "=r"(v) : : "memory");
        return v;
    }

    // EDX is read and dropped: the handover line reports the low half alone.
    static inline uint32_t read_msr_low_ordered(uint32_t index)
    {
        uint32_t lo = 0;
        uint32_t hi = 0;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(index) : "memory");
        (void)hi;
        return lo;
    }
}

#endif
