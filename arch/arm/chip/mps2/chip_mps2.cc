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
#include "crt_tail.h"
#include "semihost.h"

#include "regs.h"

#include <stdint.h>

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#include <kickos/rtt.h>
#endif

extern "C"
{
    void kickos_armv7m_init(void);

#if KICKOS_HAVE_MPU && KICKOS_ARM_MPU == KICKOS_ARM_MPU_PMSAV8
    void kickos_arm_pmsav8_init(void);
#endif

    // Read only by the SysTick ns-to-cycles conversion, which needs a consistent value and no
    // real PLL. 25 MHz is the MPS2 default.
    uint32_t SystemCoreClock = 25000000u;
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
    kickos_armv7m_init();
}

// QEMU's DWT cycle counter reads frozen, so both clocks come from SYS_CLOCK and telemetry
// timestamps here carry no latency information.
uint64_t arch_clock_now(void)
{
    static uint64_t last = 0;
    return kickos::semihost::clock_now(last);
}

uint32_t arch_trace_now(void)
{
    return static_cast<uint32_t>(arch_clock_now() / 1000ull);
}

// Replaces the WFI idle fallback: QEMU <= 10 stops the SYS_CLOCK clock while the core halts in
// WFI, so a sleep with every thread idle never wakes.
void arch_idle_wait(void)
{
    __asm volatile("nop");
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
    kickos_ranges_init();
    // Enable the FPU BEFORE running static constructors: with the hard-float ABI
    // the compiler may emit FP instructions in a global initializer, which would
    // UsageFault (CP10/CP11 disabled at reset) -> HardFault before kmain.
    kickos_armv7m_enable_fpu();
    kickos_crt_tail();
}

}
