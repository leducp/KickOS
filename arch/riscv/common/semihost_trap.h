// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_RISCV_COMMON_SEMIHOST_TRAP_H
#define KICKOS_ARCH_RISCV_COMMON_SEMIHOST_TRAP_H

namespace kickos::semihost
{
    // The slli/ebreak/srai sequence must NOT be compressed, and QEMU reads the words at ebreak-4
    // and ebreak+4: a sequence straddling a 4K page traps as a plain breakpoint. .balign 16
    // keeps the 12 bytes inside one page.
    inline __attribute__((always_inline)) long call(long op, void* arg)
    {
        register long a0 __asm("a0") = op;
        register void* a1 __asm("a1") = arg;
        __asm volatile(".option push\n"
                       ".option norvc\n"
                       ".balign 16\n"
                       "slli x0, x0, 0x1f\n"
                       "ebreak\n"
                       "srai x0, x0, 7\n"
                       ".option pop\n"
                       : "+r"(a0)
                       : "r"(a1)
                       : "memory");
        return a0;
    }

    inline __attribute__((always_inline, noreturn)) void halt_masked(void)
    {
        __asm volatile("csrci mstatus, 0x8" ::: "memory");
        while (true)
        {
            __asm volatile("wfi");
        }
    }
}

#endif
