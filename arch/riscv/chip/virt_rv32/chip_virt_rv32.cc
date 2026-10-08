// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Standard-RISC-V facts (Privileged ISA v1.10 + the QEMU `virt` memory map): CLINT
// at 0x0200_0000, with msip (hart 0) @+0x0000, mtimecmp @+0x4000, mtime @+0xBFF8; the
// `virt` mtime runs at 10 MHz. Run with: qemu-system-riscv32 -M virt -bios none
// -nographic -semihosting -kernel <elf>.
//
// Virtual board, no pads (semihosting console); arch_pinmux_set is intentionally
// left to the declining ENOSYS fallback.

#include <kickos/arch/arch.h>
#include "crt_tail.h"
#include "semihost.h"
#include <kickos/arch/clk_q32.h>

#include <stdint.h>

#include <kickos/chip_mmap.h>

extern "C"
{
    void kickos_rv32_init(void);

    // The arch layer's deferred-switch software-interrupt register (CLINT msip),
    // set here so switch.S can pend/clear it.
    extern volatile uint32_t* g_clint_msip;

    // No core clock on this machine: MTIME_HZ below is the CLINT rate, not a core rate.
    uint32_t SystemCoreClock = 0;
}

namespace
{
    inline volatile uint32_t* r32p(uintptr_t a) { return reinterpret_cast<volatile uint32_t*>(a); }

    // QEMU `virt` CLINT (hart 0).
    using kickos::virt_rv32::mmap::CLINT_BASE;
    constexpr uintptr_t CLINT_MSIP = CLINT_BASE + 0x0000;
    constexpr uintptr_t CLINT_MTIMECMP = CLINT_BASE + 0x4000; // 64-bit
    constexpr uintptr_t CLINT_MTIME = CLINT_BASE + 0xBFF8;     // 64-bit
    constexpr uint64_t MTIME_HZ = 10000000ull;
    constexpr uint64_t NS_PER_TICK = kickos::KICKOS_NS_PER_SEC / MTIME_HZ;
}

extern "C"
{

void arch_init(void)
{
    g_clint_msip = r32p(CLINT_MSIP);
    // mtimecmp = max: mtip stays low until arch_timer_arm programs a deadline.
    r32p(CLINT_MTIMECMP)[0] = 0xFFFFFFFFu;
    r32p(CLINT_MTIMECMP)[1] = 0xFFFFFFFFu;
    kickos_rv32_init();
    // No chip external interrupt controller is wired on virt (the console is
    // semihosting), so arch_irq_* stay on the arch-provided SSIP software channel.
}

uint64_t arch_clock_now(void)
{
    volatile uint32_t* mt = r32p(CLINT_MTIME);
    uint32_t hi, lo, hi2;
    // Re-read the high half to guard against a low-half rollover mid-read.
    do
    {
        hi = mt[1];
        lo = mt[0];
        hi2 = mt[1];
    } while (hi != hi2);
    uint64_t t = (static_cast<uint64_t>(hi) << 32) | lo;
    return t * NS_PER_TICK;
}

void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t ticks = deadline_ns / NS_PER_TICK;
    volatile uint32_t* cmp = r32p(CLINT_MTIMECMP);
    // Safe 64-bit write on RV32: park the high half so no spurious match can fire
    // between the two 32-bit stores, then commit lo then hi.
    cmp[1] = 0xFFFFFFFFu;
    cmp[0] = static_cast<uint32_t>(ticks);
    cmp[1] = static_cast<uint32_t>(ticks >> 32);
}

void arch_timer_disarm(void)
{
    volatile uint32_t* cmp = r32p(CLINT_MTIMECMP);
    cmp[0] = 0xFFFFFFFFu;
    cmp[1] = 0xFFFFFFFFu;
}

void arch_shutdown(int status)
{
    kickos::semihost::exit(status);
}

void Reset_Handler(void)
{
    kickos_ranges_init();
    kickos_crt_tail();
}

}
