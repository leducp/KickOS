// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// mps2-an386 (QEMU Cortex-M4F) chip backend: the hardware edges the armv7m arch
// layer leaves to the chip -- reset/C-runtime bring-up, arch_init (FPU + core
// exception/clock setup), and the debug console. QEMU semihosting stands in for
// a UART here (console + exit code), so this target needs no peripheral driver;
// the K64F chip layer (item 10) swaps semihosting for real MCG clocks + UART0.

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    // From the armv7m arch layer: installs SHPR priorities + enables DWT.
    void kickos_armv7m_init(void);

    // Linker-script symbols (mps2.ld).
    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // CMSIS convention: core clock in Hz. QEMU's Cortex-M4 has no real PLL; the
    // DWT-based clock only needs a consistent value. 25 MHz is the MPS2 default.
    uint32_t SystemCoreClock = 25000000u;
}

namespace
{
    // ARM semihosting call (QEMU implements the host side).
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

    void enable_fpu()
    {
        // CPACR: grant full access to CP10/CP11 (the FPU). Without this, any FP
        // instruction the compiler emits would UsageFault.
        volatile uint32_t* cpacr = reinterpret_cast<volatile uint32_t*>(0xE000ED88);
        *cpacr |= (0xFu << 20);
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }

    void run_init_array()
    {
        for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
        {
            (*fn)();
        }
    }
}

extern "C"
{

void arch_init(void)
{
    // FPU is enabled earlier (Reset_Handler, before C++ ctors) -- see there.
    kickos_armv7m_init();
}

// Override the arch layer's weak DWT clock: QEMU does not implement the DWT
// cycle counter (it reads frozen), so derive the monotonic clock from the
// semihosting SYS_CLOCK (centiseconds since start). Coarse (10 ms) -- fine for
// the QEMU functional gate; real silicon uses the DWT default.
uint64_t arch_clock_now(void)
{
    // Must be monotonic: a semihosting error/glitch (cs < 0) must not regress the
    // clock to 0 (which would stall every armed sleeper). Clamp to the last value.
    // Guard the RMW: `last` is 64-bit on a 32-bit core and shared thread<->ISR,
    // so a torn store latched by the clamp would jump the clock forward forever.
    static uint64_t last = 0;
    arch_irq_state_t st = arch_irq_save();
    long cs = semihost(SYS_CLOCK, nullptr);
    uint64_t ns = 0;
    if (cs > 0)
    {
        ns = static_cast<uint64_t>(cs) * 10000000ull; // 1 cs = 10 ms = 1e7 ns
    }
    if (ns < last)
    {
        ns = last;
    }
    last = ns;
    arch_irq_restore(st);
    return ns;
}

void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        char c = buf[i];
        semihost(SYS_WRITEC, &c);
    }
}

void arch_shutdown(int status)
{
    uint32_t block[2];
    block[0] = ADP_Stopped_ApplicationExit;
    block[1] = static_cast<uint32_t>(status);
    semihost(SYS_EXIT_EXTENDED, block);
    // Semihosting exit should terminate QEMU; if it returns, halt.
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

// C-runtime bring-up (the reset vector). Runs on MSP; touches only linker
// symbols until .data/.bss are live.
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
    // Enable the FPU BEFORE running static constructors: with the hard-float ABI
    // the compiler may emit FP instructions in a global initializer, which would
    // UsageFault (CP10/CP11 disabled at reset) -> HardFault before kmain.
    enable_fpu();
    run_init_array();
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0); // kmain returns only if the scheduler unwinds to boot
}

void HardFault_Handler(void)
{
    // No MPU on M1: a fault here is a genuine bug. Exit loudly with a distinct
    // code so a QEMU/CTest run reports it rather than hanging silently.
    arch_shutdown(132);
}

}
