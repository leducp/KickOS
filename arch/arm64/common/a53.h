// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The Cortex-A53 facts both arm64 chips rest on: the kernel's device window, the semihosting
// call and the timer PPI. A chip keeps its UART, its reserved-block list, its EL handover and
// its reset path.

#ifndef KICKOS_ARCH_ARM64_COMMON_A53_H
#define KICKOS_ARCH_ARM64_COMMON_A53_H

#include <stdint.h>

extern "C"
{
    // VA - PA for the kernel's half, defined by the chip linker script.
    extern unsigned char __kickos_arm64_va_base[];

    // Seats the tick conversion arch_clock_now and arch_timer_arm read, from CNTFRQ_EL0.
    // Returns the frequency, or 0 when it does not divide a second exactly and the conversion
    // would be lossy. CNTFRQ_EL0 is firmware-programmed, so 0 also covers a handover that left
    // it unwritten. The caller refuses in its own name.
    uint64_t kickos_armv8a_timebase_init(void);

    // This core's hardware edge alone: the distributor's shared half runs once for the machine.
    // A core reaches this with PSTATE.DAIF masked.
    void kickos_armv8a_percore_init(void);
}

namespace kickos::arm64
{
    // EVERY DEVICE REGISTER IS REACHED THROUGH THE KERNEL'S OWN HALF. The device gigabyte is
    // mapped at PA + __kickos_arm64_va_base by TTBR1, which every address space shares; TTBR0
    // carries a per-process root that maps no device at all, so a low literal here would
    // translate against whatever process happened to be running.
    inline uintptr_t dev_va(uintptr_t pa)
    {
        return pa + reinterpret_cast<uintptr_t>(__kickos_arm64_va_base);
    }

    inline volatile uint32_t* r32p(uintptr_t a)
    {
        return reinterpret_cast<volatile uint32_t*>(dev_va(a));
    }

    // The Non-secure EL1 physical timer. ARCHITECTURALLY ASSIGNED AND NOT A CHIP FACT: neither
    // reference manual documents a PPI number, so this rests on the GIC architecture. It is NOT
    // a kernel IRQ line: kickos_isr_timer takes no line and the timer is in no dispatch table.
    constexpr int PPI_EL1_PHYS_TIMER = 30;

    constexpr long SYS_EXIT = 0x18;
    constexpr uint64_t ADP_Stopped_ApplicationExit = 0x20026u;

    // AArch64 semihosting: `hlt #0xF000` with the operation in x0 and the parameter in x1.
    // ALWAYS inlined: a caller in the boot span can call nothing outside it.
    inline __attribute__((always_inline)) long semihost(long op, void* arg)
    {
        register long x0 __asm("x0") = op;
        register void* x1 __asm("x1") = arg;
        __asm volatile("hlt #0xF000" : "+r"(x0) : "r"(x1) : "memory");
        return x0;
    }

    // Where a dead end ends when nothing listens for semihosting. ALWAYS inlined for the same
    // boot-span caller, and noreturn because arch.h declares arch_shutdown so.
    inline __attribute__((always_inline, noreturn)) void halt_masked(void)
    {
        __asm volatile("msr daifset, #0xf" ::: "memory");
        while (true)
        {
            __asm volatile("wfi");
        }
    }
}

#endif
