// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// nRF51822 (BBC micro:bit v1, Cortex-M0) chip backend -- the runnable armv6m
// verification target under QEMU (-M microbit). Like the mps2 chip, it uses ARM
// semihosting for the console/clock/exit, so it needs no UART driver. The nRF51
// watchdog is off at reset (unlike the K64F) and Cortex-M0 has no FPU, so the
// reset path is just C-runtime init.
//
// SCOPE: this is a QEMU validation vehicle for the armv6m arch layer, not a
// hardware product target. Two things QEMU models that the REAL nRF51 M0 does
// not: (1) SysTick -- the Cortex-M0 in the nRF51 is built without it (Nordic
// uses the RTC), so arch_timer_arm (SysTick, in the arch layer) is QEMU-only
// here; a real micro:bit would need an RTC-based timer in this chip layer.
// (2) unprivileged execution -- the M0 has no Unpriv/Priv extension (M0+ does),
// so the nPRIV separation runs on QEMU but degrades to all-privileged on the
// real M0. The real v6-M privilege+timer target is the RP2040 (M0+).

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_armv6m_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // nRF51 runs at 16 MHz; only used for the SysTick ns->cycles conversion.
    uint32_t SystemCoreClock = 16000000u;
}

namespace
{
    inline long semihost(long op, void* arg)
    {
        register long r0 __asm("r0") = op;
        register void* r1 __asm("r1") = arg;
        __asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
        return r0;
    }

    constexpr long SYS_WRITEC = 0x03;
    constexpr long SYS_CLOCK = 0x10;
    constexpr long SYS_EXIT_EXTENDED = 0x20;
    constexpr uint32_t ADP_Stopped_ApplicationExit = 0x20026u;
}

extern "C"
{

void arch_init(void)
{
    kickos_armv6m_init();
}

void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        char c = buf[i];
        semihost(SYS_WRITEC, &c);
    }
}

// v6-M has no DWT; derive the monotonic clock from semihosting SYS_CLOCK
// (centiseconds). Coarse (10 ms) but fine for the functional gate; monotonic-
// clamped so a semihosting glitch can't stall armed sleepers.
uint64_t arch_clock_now(void)
{
    // The monotonic-clamp RMW of `last` is shared between thread and ISR context
    // and `last` is 64-bit on a 32-bit core (non-atomic), so guard it -- else a
    // torn store latched by the clamp jumps the clock forward permanently.
    static uint64_t last = 0;
    arch_irq_state_t st = arch_irq_save();
    long cs = semihost(SYS_CLOCK, nullptr);
    uint64_t ns = 0;
    if (cs > 0)
    {
        ns = static_cast<uint64_t>(cs) * 10000000ull;
    }
    if (ns < last)
    {
        ns = last;
    }
    last = ns;
    arch_irq_restore(st);
    return ns;
}

void arch_shutdown(int status)
{
    uint32_t block[2];
    block[0] = ADP_Stopped_ApplicationExit;
    block[1] = static_cast<uint32_t>(status);
    semihost(SYS_EXIT_EXTENDED, block);
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
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
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

void HardFault_Handler(void)
{
    arch_shutdown(132);
}

}
