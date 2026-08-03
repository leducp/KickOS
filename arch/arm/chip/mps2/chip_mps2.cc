// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// mps2-an386 (QEMU Cortex-M4F) chip backend: the hardware edges the armv7m arch
// layer leaves to the chip, namely reset/C-runtime bring-up, arch_init (FPU + core
// exception/clock setup), and the debug console. QEMU semihosting stands in for a
// UART here (console + exit code), so this target needs no peripheral driver.
//
// The emulated CMSDK has no pin-function mux; arch_pinmux_set is intentionally
// left to the declining ENOSYS fallback.

#include <kickos/arch/arch.h>

#include "regs.h" // arch/arm/common: kickos_armv7m_enable_fpu + core SCB regs

#include <stdint.h>

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#include <kickos/rtt.h>
#endif

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    // From the armv7m arch layer: installs SHPR priorities + enables DWT.
    void kickos_armv7m_init(void);

    // PMSAv8 MPU backend (arch/arm/common/arch_arm_pmsav8.cc): one-time MAIR +
    // MemManage enable. Also the LINK ANCHOR that pulls the PMSAv8 archive member,
    // so its commit/encodable replace the v7-M fallback TUs. Only the M33
    // board (mps2-an505) defines KICKOS_MPS2_PMSAV8; see the chip's mpu.cmake.
#if KICKOS_HAVE_MPU && defined(KICKOS_MPS2_PMSAV8)
    void kickos_arm_pmsav8_init(void);
#endif

    // Linker-script symbols (mps2.ld).
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

    constexpr long SYS_OPEN = 0x01;
    constexpr long SYS_CLOSE = 0x02;
    constexpr long SYS_WRITE = 0x05;
    constexpr long SYS_WRITEC = 0x03;
    constexpr long SYS_CLOCK = 0x10;
    constexpr long SYS_EXIT_EXTENDED = 0x20;
    constexpr uint32_t ADP_Stopped_ApplicationExit = 0x20026u;

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
    // FPU is enabled earlier (Reset_Handler, before C++ ctors).
#if KICKOS_HAVE_MPU && defined(KICKOS_MPS2_PMSAV8)
    // MUST precede kickos_armv7m_init and MUST NOT be dropped: this reference pulls
    // the PMSAv8 backend into the link. Without it the build still succeeds, but the
    // PMSAv7 commit fallback stands and writes RASR values into what is RLAR on v8-M.
    kickos_arm_pmsav8_init(); // MAIR + MemManage; the first switch enables the MPU
#endif
    kickos_armv7m_init();
}

// The required per-chip clock. QEMU does not implement the DWT cycle counter (it reads
// frozen), so derive the monotonic clock from the semihosting SYS_CLOCK (centiseconds
// since start). Resolution is 10 ms.
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

// Replaces the arch layer's DWT trace-clock fallback: QEMU's DWT CYCCNT reads frozen,
// so derive the telemetry timestamp from the same monotonic semihosting clock. 10 ms
// resolution, so timestamps here carry no latency information.
uint32_t arch_trace_now(void)
{
    return static_cast<uint32_t>(arch_clock_now() / 1000ull); // us
}

// Replaces the arch layer's WFI idle fallback: this target's clock is the semihosting
// SYS_CLOCK (arch_clock_now above), and QEMU <= 10 stops it while the core halts
// in WFI, so a sleep with every thread idle never wakes. Spin instead.
void arch_idle_wait(void)
{
    __asm volatile("nop");
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
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Mask IRQs across the ENTIRE flush (root_entry -> arch_shutdown holds no
    // lock): a SysTick landing here would emit records after the closing SESSION's
    // records_attempted snapshot, breaking the decoder cross-check. Held to exit.
    (void)arch_irq_save();
    // Capture the ch1 telemetry ring to a host file via semihosting (QEMU writes
    // it relative to its CWD) so the offline decoder gets the trace. Best-effort on
    // this dying path.
    kickos_trace_final_session();
    kickos_trace_report_counters();
    {
        char const name[] = "kicktrace.bin";
        uint32_t oparm[3] = {reinterpret_cast<uint32_t>(name), 5 /* "wb" */,
                             sizeof(name) - 1};
        long fh = semihost(SYS_OPEN, oparm);
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
                semihost(SYS_WRITE, wparm);
            }
            uint32_t cparm[1] = {static_cast<uint32_t>(fh)};
            semihost(SYS_CLOSE, cparm);
        }
    }
#endif
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

// A fault/panic on this QEMU target must EXIT with a status so a CTest run catches
// it: there is no LED, and the blink terminal fallback (kernel.h) would just spin
// until the harness times out.
void kfault_terminate(void)
{
    arch_shutdown(132);
}

#if KICKOS_HAVE_MPU
// Rule 7 reserved set: empty. Console and time base are semihosting calls, and the
// only registers touched (SysTick/NVIC/SCB) live in the PPB, which the MPU does not
// govern. KICKOS_RESERVED_NONE is legal per arch.h.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    (void)out;
    (void)max;
    return KICKOS_RESERVED_NONE;
}
#endif

void Reset_Handler(void)
{
    // init .data + (under enforcement) the pow2 app-data block; zero .bss + app-bss
    kickos_ranges_init();
    // Enable the FPU BEFORE running static constructors: with the hard-float ABI
    // the compiler may emit FP instructions in a global initializer, which would
    // UsageFault (CP10/CP11 disabled at reset) -> HardFault before kmain.
    kickos_armv7m_enable_fpu();
    run_init_array();
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0); // kmain returns only if the scheduler unwinds to boot
}

}
