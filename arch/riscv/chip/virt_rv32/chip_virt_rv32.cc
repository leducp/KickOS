// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// QEMU `virt` RV32 chip: CLINT (msip, mtimecmp, mtime at 10 MHz) and a semihosting console.
// Run with: qemu-system-riscv32 -M virt -bios none -nographic -semihosting -kernel <elf>.

#include <kickos/arch/arch.h>
#include <arch_reboot_tail.h>
#include <kickos/diag.h>
#include "clint_rv32.h"
#include "crt_tail.h"
#include "semihost.h"
#include <kickos/arch/clk_q32.h>
#include <kickos/io/mmio.h>

#include <stdint.h>

#include <kickos/chip_limits.h>
#include <kickos/chip_mmap.h>
#include <kickos/sys/abi.h>

extern "C"
{
    void kickos_rv32_init(void);

    // CLINT msip, which switch.S pends and clears.
    extern volatile uint32_t* g_clint_msip;

    uint32_t SystemCoreClock = 0;
}

namespace
{
    // QEMU `virt` CLINT (hart 0).
    using kickos::virt_rv32::mmap::CLINT_BASE;
    constexpr uintptr_t CLINT_MSIP = CLINT_BASE + 0x0000;
    constexpr uintptr_t CLINT_MTIMECMP = CLINT_BASE + 0x4000; // 64-bit
    constexpr uintptr_t CLINT_MTIME = CLINT_BASE + 0xBFF8;     // 64-bit
    constexpr uint64_t MTIME_HZ = KICKOS_CHIP_TIMEBASE_HZ;
    constexpr uint64_t NS_PER_TICK = kickos::KICKOS_NS_PER_SEC / MTIME_HZ;
}

extern "C"
{

void arch_init(void)
{
    g_clint_msip = &r32(CLINT_MSIP);
    // mtip stays low until arch_timer_arm programs a deadline.
    kickos::rv32::clint::disarm(CLINT_MTIMECMP);
    kickos_rv32_init();
    // No external interrupt controller: arch_irq_* use the arch SSIP channel.
}

uint64_t arch_clock_now(void)
{
    return kickos::rv32::clint::read(CLINT_MTIME) * NS_PER_TICK;
}

void arch_timer_arm(uint64_t deadline_ns)
{
    kickos::rv32::clint::arm(CLINT_MTIMECMP, deadline_ns / NS_PER_TICK);
}

void arch_timer_disarm(void)
{
    kickos::rv32::clint::disarm(CLINT_MTIMECMP);
}

void arch_shutdown(int status)
{
    kickos::semihost::exit(status);
}

// The SiFive test finisher's reset word restarts the machine.
int arch_reboot(uint32_t outcome)
{
    if (outcome != KOS_SHUTDOWN_REBOOT)
    {
        return -KOS_ENOSYS;
    }
    constexpr uint32_t FINISHER_RESET = 0x7777;
    __asm volatile("csrci mstatus, 0x8" ::: "memory");
    r32(kickos::virt_rv32::mmap::TEST_FINISHER_BASE) = FINISHER_RESET;
    kickos::arch::reset_settle();
    kickos::arch::reboot_failed(kickos::diag::kResetNotTaken);
}

void Reset_Handler(void)
{
    kickos_ranges_init();
    kickos_crt_tail();
}

}
