// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Infineon XMC4800 (XMC4800 Relax Kit, Cortex-M4F) chip backend. Registers
// clean-room from the XMC4700/XMC4800 Reference Manual; hand-rolled, no XMCLib.
//
// M1 scope: privilege + SVC, no MPU. NO PLL: the core boots on the internal
// backup clock fOFI (~24 MHz) with no SCU config, and the watchdog is OFF at
// reset (WDT_CTR.ENB = 0), so the reset path is just FPU + C-runtime + VTOR.
// Code/vectors are linked at the cached flash alias 0x0800_0000.
//
// Console: the XMC serial peripheral is the USIC, which is involved to bring up;
// for this first "does the tree fit" target the console is a no-op stub (a USIC
// UART is a follow-up). apps/blink toggles LED1 (P5.9) -- the actual smoke test.
// The XMC4800 also carries an on-chip EtherCAT node, a natural future KickCAT
// target once the console/timers are fleshed out.
//
// Build-only here; flash via the on-board debugger.

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_armv7m_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    uint32_t SystemCoreClock = 24000000u; // fOFI backup clock at reset, no PLL
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    constexpr uintptr_t SCB_VTOR = 0xE000ED08;
    constexpr uintptr_t FLASH_BASE = 0x08000000; // cached flash alias (link base)

    void enable_fpu()
    {
        r32(0xE000ED88) |= (0xFu << 20); // CPACR: CP10/CP11 full access
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }
}

extern "C"
{

void arch_init(void)
{
    // fOFI is already the system clock and the WDT is off at reset -- only the
    // NVIC/SHPR priorities need installing.
    kickos_armv7m_init();
}

// USIC UART not yet brought up on this target; console output is dropped.
void arch_console_write(char const* buf, size_t n)
{
    (void)buf;
    (void)n;
}

void arch_shutdown(int status)
{
    (void)status;
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    enable_fpu();
    r32(SCB_VTOR) = FLASH_BASE; // vectors live at the cached flash alias

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
