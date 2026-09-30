// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Check the x86-64-v3 floor before running code built for it. This file is compiled for the
// base level so an older processor can report the refusal through firmware and return safely.

#include <kickos/arch/uefi.h>

#include <stdint.h>

#if defined(__BMI__) || defined(__BMI2__) || defined(__LZCNT__) || defined(__MOVBE__) \
    || defined(__POPCNT__)
#error "floor_x86_64.cc runs below the floor and must be compiled for the base level"
#endif

namespace
{
    struct Regs
    {
        uint32_t a;
        uint32_t b;
        uint32_t c;
        uint32_t d;
    };

    Regs cpuid(uint32_t leaf)
    {
        Regs r = {0, 0, 0, 0};
        __asm__ volatile("cpuid"
                         : "=a"(r.a), "=b"(r.b), "=c"(r.c), "=d"(r.d)
                         : "a"(leaf), "c"(0u));
        return r;
    }

    // CPUID features required through x86-64-v3. The kernel disables vector state separately.
    // Leaf 1 EDX: FPU, CX8, CMOV, MMX, FXSR, SSE, SSE2.
    constexpr uint32_t leaf1_edx = (1u << 0) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24)
                                   | (1u << 25) | (1u << 26);
    // Leaf 1 ECX: SSE3, SSSE3, FMA, CMPXCHG16B, SSE4.1, SSE4.2, MOVBE, POPCNT, XSAVE, AVX, F16C.
    constexpr uint32_t leaf1_ecx = (1u << 0) | (1u << 9) | (1u << 12) | (1u << 13) | (1u << 19)
                                   | (1u << 20) | (1u << 22) | (1u << 23) | (1u << 26) | (1u << 28)
                                   | (1u << 29);
    // Leaf 7 EBX: BMI1, AVX2, BMI2.
    constexpr uint32_t leaf7_ebx = (1u << 3) | (1u << 5) | (1u << 8);
    // Leaf 0x80000001 ECX: LAHF/SAHF, LZCNT. EDX: SYSCALL/SYSRET.
    constexpr uint32_t ext1_ecx = (1u << 0) | (1u << 5);
    constexpr uint32_t ext1_edx = 1u << 11;

    bool floor_met(void)
    {
        if (cpuid(0).a < 7 or cpuid(0x80000000u).a < 0x80000001u)
        {
            return false;
        }
        Regs const l1 = cpuid(1);
        Regs const l7 = cpuid(7);
        Regs const e1 = cpuid(0x80000001u);
        return (l1.d & leaf1_edx) == leaf1_edx and (l1.c & leaf1_ecx) == leaf1_ecx
               and (l7.b & leaf7_ebx) == leaf7_ebx and (e1.c & ext1_ecx) == ext1_ecx
               and (e1.d & ext1_edx) == ext1_edx;
    }
}

extern "C" __attribute__((visibility("hidden"))) bool
kickos_x86_64_floor(kickos::uefi::system_table* systab)
{
    if (floor_met())
    {
        return true;
    }
    if (systab != nullptr and systab->con_out != nullptr)
    {
        systab->con_out->output_string(systab->con_out,
                                       u"KickOS: this processor is below x86-64-v3, the floor "
                                       u"it is built for\r\n");
    }
    return false;
}
