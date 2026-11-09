// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// nRF51822 (BBC micro:bit v1, Cortex-M0) chip backend, the armv6m verification target under QEMU
// (-M microbit), with ARM semihosting for console and exit.
//
// SCOPE: a QEMU validation vehicle for the armv6m arch layer. QEMU models two things the real
// nRF51 M0 lacks:
// (1) SysTick: the nRF51's M0 is built without it (Nordic uses the RTC), so arch_timer_arm is
// QEMU-only; hardware would need an RTC-based timer in this chip layer.
// (2) unprivileged execution: the M0 has no Unpriv/Priv extension (M0+ does), so the nPRIV
// separation runs on QEMU and degrades to all-privileged on the real part.
//
// Routing is per-peripheral PSEL, so arch_pinmux_set is the declining ENOSYS fallback.

#include <kickos/arch/arch.h>
#include <kickos/arch/wrap_extend.h>
#include <kickos/chip_limits.h>
#include <kickos/io/mmio.h>
#include <kickos/units.h>
#include <chip_layout.h>
#include "crt_tail.h"
#include "semihost.h"

#include "regs/timer.h"

#include <stdint.h>

extern "C"
{
    void kickos_armv6m_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

    // Read only by the SysTick ns-to-cycles conversion.
    uint32_t SystemCoreClock = 16000000u;
}

namespace
{
    namespace tmr = kickos::nrf51::reg::timer;
    using namespace kickos::units;

    static_assert(1_s % KICKOS_CHIP_TIMEBASE_HZ == 0, "arch_clock_now scales by whole ns per tick");
    constexpr uint64_t NS_PER_TICK = 1_s / KICKOS_CHIP_TIMEBASE_HZ;

    // The counter has no register of its own: a capture copies it into CC[0]. A reader
    // preempted between the two accesses reads the preempter's later capture, which is still a
    // count taken inside its own read. COMPARE[3] against a CC[3] of 0 is the wrap.
    struct timer0_wrap_ops
    {
        static uint32_t count()
        {
            r32(tmr::TASKS_CAPTURE0) = 1u;
            return r32(tmr::CC0);
        }
        static bool pending() { return r32(tmr::EVENTS_COMPARE3) != 0; }
        static bool take()
        {
            if (not pending())
            {
                return false;
            }
            r32(tmr::EVENTS_COMPARE3) = 0u;
            return true;
        }
    };
    kickos::arch_wrap_extend<timer0_wrap_ops> g_clk_wrap;

    // PRESCALER and BITMODE are written only while the timer is stopped (sec.18.1).
    void timer0_clock_init()
    {
        r32(tmr::TASKS_STOP) = 1u;
        r32(tmr::MODE) = tmr::MODE_TIMER;
        r32(tmr::BITMODE) = tmr::BITMODE_32;
        r32(tmr::PRESCALER) = tmr::PRESCALER_1MHZ;
        r32(tmr::CC3) = 0u;
        r32(tmr::TASKS_CLEAR) = 1u;
        r32(tmr::EVENTS_COMPARE3) = 0u;
        r32(tmr::INTENSET) = tmr::INTEN_COMPARE3;
        r32(tmr::TASKS_START) = 1u;
        arch_irq_unmask(KICKOS_LAYOUT_LINE_TIMER0_IRQ);
    }
}

extern "C"
{

void arch_init(void)
{
    timer0_clock_init();
    kickos_armv6m_init();
}

uint64_t arch_clock_now(void)
{
    return g_clk_wrap.read() * NS_PER_TICK;
}

void kickos_nrf51_clock_isr(void)
{
    g_clk_wrap.on_overflow();
}

// v6-M has no DWT, so the trace clock is the low word of the clock's own count.
uint32_t arch_trace_now(void)
{
    return timer0_wrap_ops::count();
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
    kickos_crt_images_data();
    kickos_crt_tail();
}

}
