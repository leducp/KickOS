// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The Cortex-A53 facts both arm64 chips rest on: the kernel's device window, the semihosting
// call, the EL refusal and the C runtime ranges.

#ifndef KICKOS_ARCH_ARM64_COMMON_A53_H
#define KICKOS_ARCH_ARM64_COMMON_A53_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/gic_ppi.h>

#include <fatal_status.ld.h>

extern "C"
{
    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    // The app's .bss lives in the low window, outside _sbss.._ebss, so the boot zeroing
    // covers it separately. Its .data needs no copy: the low window links VMA == LMA.
    extern uint32_t __kickos_appbss_start, __kickos_appbss_end;

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

    // Panics where this core's MMU cannot run the port's translation regime.
    void kickos_armv8a_mmu_check(void);
}

namespace kickos::arm64
{
    // Every device register is reached through the kernel's half (TTBR1, PA +
    // __kickos_arm64_va_base): TTBR0's per-process root maps no device, so a low literal would
    // translate against whatever process is running.
    inline uintptr_t dev_va(uintptr_t pa)
    {
        return pa + reinterpret_cast<uintptr_t>(__kickos_arm64_va_base);
    }

    inline volatile uint32_t* r32p(uintptr_t a)
    {
        return reinterpret_cast<volatile uint32_t*>(dev_va(a));
    }

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

    using boot_write_fn = void (*)(char const* buf, size_t n);

    // The body of a chip's kickos_arm64_bad_el: head, the entry level, tail, then exit. write is
    // the chip's boot-span writer and the strings sit in the boot span too, since this runs
    // before .data and .bss exist and with the MMU off.
    inline __attribute__((always_inline, noreturn)) void bad_el_exit(unsigned long el,
                                                                     boot_write_fn write,
                                                                     char const* head, size_t head_n,
                                                                     char const* tail, size_t tail_n)
    {
        char const digit = static_cast<char>('0' + (el & 3));
        write(head, head_n);
        write(&digit, 1);
        write(tail, tail_n);

        // 16-byte aligned: every access is Device-nGnRnE while the MMU is off, which faults on
        // an unaligned one whatever SCTLR_EL1.A says, and the compiler stores this pair wide.
        uint64_t block[2] __attribute__((aligned(16)));
        block[0] = ADP_Stopped_ApplicationExit;
        block[1] = KICKOS_FATAL_STATUS;
        semihost(SYS_EXIT, block);

        halt_masked();
    }

    // Copies .data and zeroes both .bss ranges, ahead of the constructors.
    inline __attribute__((always_inline)) void crt_ranges_init(void)
    {
        uint32_t* src = &_sidata;
        uint32_t* dst = &_sdata;
        while (dst < &_edata)
        {
            *dst++ = *src++;
        }
        for (uint32_t* b = &_sbss; b < &_ebss; b++)
        {
            *b = 0;
        }
        for (uint32_t* b = &__kickos_appbss_start; b < &__kickos_appbss_end; b++)
        {
            *b = 0;
        }
    }
}

#endif
