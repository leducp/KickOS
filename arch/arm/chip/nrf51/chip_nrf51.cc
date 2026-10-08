// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// nRF51822 (BBC micro:bit v1, Cortex-M0) chip backend, the runnable armv6m
// verification target under QEMU (-M microbit). It uses ARM semihosting for the
// console/clock/exit, so it needs no UART driver. The nRF51 watchdog is off at reset
// and Cortex-M0 has no FPU, so the reset path is just C-runtime init.
//
// SCOPE: a QEMU validation vehicle for the armv6m arch layer, not a hardware product
// target. Two things QEMU models that the REAL nRF51 M0 does not:
// (1) SysTick: the Cortex-M0 in the nRF51 is built without it (Nordic uses the RTC),
// so arch_timer_arm (SysTick, in the arch layer) is QEMU-only here; a real micro:bit
// would need an RTC-based timer in this chip layer.
// (2) unprivileged execution: the M0 has no Unpriv/Priv extension (M0+ does), so the
// nPRIV separation runs on QEMU but degrades to all-privileged on the real M0.
//
// No central pinmux: routing is per-peripheral PSEL, so arch_pinmux_set is left to
// the declining ENOSYS fallback.

#include <kickos/arch/arch.h>
#include "crt_tail.h"
#include "semihost.h"

#include <stdint.h>

extern "C"
{
    void kickos_armv6m_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

    // Read only by the SysTick ns-to-cycles conversion.
    uint32_t SystemCoreClock = 16000000u;
}

extern "C"
{

void arch_init(void)
{
    kickos_armv6m_init();
}

// v6-M has no DWT, so both clocks come from SYS_CLOCK.
uint64_t arch_clock_now(void)
{
    static uint64_t last = 0;
    return kickos::semihost::clock_now(last);
}

uint32_t arch_trace_now(void)
{
    return static_cast<uint32_t>(arch_clock_now() / 1000ull);
}

// The Cortex-M0 nRF51 has no MPU, unlike the M0+ RP2040: 0 is the seam's no-MPU
// granule (arch.h), not a tuning choice.
size_t arch_mpu_min_region(void)
{
    return 0u;
}

// Replaces the WFI idle fallback: QEMU <= 10 stops the SYS_CLOCK clock while the core halts in
// WFI, so a sleep with every thread idle never wakes.
void arch_idle_wait(void)
{
    __asm volatile("nop");
}

void arch_shutdown(int status)
{
    kickos::semihost::exit(status);
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
    kickos_crt_tail();
}

}
