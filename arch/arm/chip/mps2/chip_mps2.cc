// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The QEMU MPS2 machines' chip backend, compiled into the mps2 chip's archive and, as its
// family, into the an505 chip's: the hardware edges the armv7m arch layer leaves to the chip.
// QEMU semihosting carries the console and the exit code.
//
// The emulated CMSDK has no pin-function mux, so arch_pinmux_set is left to the
// declining ENOSYS fallback.

#include <kickos/arch/arch.h>
#include <kickos/arch/wrap_extend.h>
#include <kickos/chip_limits.h>
#include <kickos/io/mmio.h>
#include <kickos/units.h>
#include <chip_layout.h>
#include "crt_tail.h"
#include "semihost.h"

#include "regs.h"
#include "regs/dualtimer.h"

#include <stdint.h>

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#include <kickos/rtt.h>
#endif

extern "C"
{
    void kickos_armv7m_ccr_init(void);
    void kickos_armv7m_init(void);

#if KICKOS_HAVE_MPU && KICKOS_ARM_MPU == KICKOS_ARM_MPU_PMSAV8
    void kickos_arm_pmsav8_init(void);
#endif

    // SysTick counts the SYSCLK the dual timer counts.
    uint32_t SystemCoreClock = KICKOS_CHIP_TIMEBASE_HZ;
}

namespace
{
    namespace dt = kickos::mps2::reg::dualtimer;
    using namespace kickos::units;

    static_assert(1_s % KICKOS_CHIP_TIMEBASE_HZ == 0, "arch_clock_now scales by whole ns per tick");
    constexpr uint64_t NS_PER_TICK = 1_s / KICKOS_CHIP_TIMEBASE_HZ;

    // A down-counter, so the count is its complement. The interrupt latches as the count
    // reaches zero, one tick before the wrap.
    struct dualtimer_wrap_ops
    {
        static uint32_t count() { return ~r32(dt::TIMER1VALUE); }
        static bool pending() { return (r32(dt::TIMER1RIS) & dt::RIS_INT) != 0; }
        static bool take()
        {
            if (not pending())
            {
                return false;
            }
            r32(dt::TIMER1INTCLR) = 1u;
            return true;
        }
    };
    kickos::arch_wrap_extend<dualtimer_wrap_ops> g_clk_wrap;

    void dualtimer_clock_init()
    {
        r32(dt::TIMER1CONTROL) = 0u;
        r32(dt::TIMER1LOAD) = 0xFFFFFFFFu;
        r32(dt::TIMER1INTCLR) = 1u;
        r32(dt::TIMER1CONTROL) = dt::CONTROL_ENABLE | dt::CONTROL_INTENABLE | dt::CONTROL_SIZE32;
        arch_irq_unmask(KICKOS_LAYOUT_LINE_DUALTIMER_IRQ);
    }
}

extern "C"
{

void arch_init(void)
{
#if KICKOS_HAVE_MPU && KICKOS_ARM_MPU == KICKOS_ARM_MPU_PMSAV8
    // MUST precede kickos_armv7m_init and MUST NOT be dropped: this reference pulls
    // the PMSAv8 backend into the link. Without it the build still succeeds, but the
    // PMSAv7 commit fallback stands and writes RASR values into what is RLAR on v8-M.
    kickos_arm_pmsav8_init();
#endif
    dualtimer_clock_init();
    kickos_armv7m_init();
}

uint64_t arch_clock_now(void)
{
    return g_clk_wrap.read() * NS_PER_TICK;
}

void kickos_mps2_clock_isr(void)
{
    g_clk_wrap.on_overflow();
}

// QEMU's DWT reads zero, so the trace clock is the low word of the clock's own count.
uint32_t arch_trace_now(void)
{
    return dualtimer_wrap_ops::count();
}

void arch_shutdown(int status)
{
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Masked to the exit: a SysTick here would emit records after the closing SESSION's
    // records_attempted snapshot, breaking the decoder cross-check.
    (void)arch_irq_save();
    // The ch1 ring goes to kicktrace.bin, relative to QEMU's working directory.
    kickos_trace_final_session();
    kickos_trace_report_counters();
    {
        constexpr long SYS_OPEN = 0x01;
        constexpr long SYS_CLOSE = 0x02;
        constexpr long SYS_WRITE = 0x05;
        char const name[] = "kicktrace.bin";
        uint32_t oparm[3] = {reinterpret_cast<uint32_t>(name), 5 /* "wb" */,
                             sizeof(name) - 1};
        long fh = kickos::semihost::call(SYS_OPEN, oparm);
        if (fh > 0)
        {
            char buf[256];
            while (true)
            {
                size_t got = kickos_rtt_ch1_drain(buf, sizeof(buf));
                if (got == 0)
                {
                    break;
                }
                uint32_t wparm[3] = {static_cast<uint32_t>(fh),
                                     reinterpret_cast<uint32_t>(buf),
                                     static_cast<uint32_t>(got)};
                kickos::semihost::call(SYS_WRITE, wparm);
            }
            uint32_t cparm[1] = {static_cast<uint32_t>(fh)};
            kickos::semihost::call(SYS_CLOSE, cparm);
        }
    }
#endif
    kickos::semihost::exit(status);
}

void Reset_Handler(void)
{
    kickos_armv7m_ccr_init();
    kickos_armv7m_enable_fpu(); // before any code a hard-float ABI might emit FP into
    kickos_ranges_init();
    kickos_crt_tail();
}

}
