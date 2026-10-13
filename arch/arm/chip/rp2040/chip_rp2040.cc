// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi RP2040 (Pico), Cortex-M0+ chip backend (datasheet RP-008371-DS).
//
// clk_sys runs at 125 MHz off PLL_SYS (12 MHz XOSC x125 /6 /2). boot2.S runs BEFORE this file:
// Reset_Handler already executes in place from flash.

#include <kickos/arch/arch.h>
#include <arch_reboot_tail.h>
#include "crt_tail.h"
#include <kickos/diag.h>
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h>
#include <kickos/usb_console.h>

#include <stdint.h>

#include "board_pins.h"
#include "irq.h"
#include "regs/io_bank0.h"
#include "regs/resets.h"
#include "regs/sio.h"
#include "regs/watchdog.h"
#include "../rp2xxx/bringup_values.h"
#include "../rp2xxx/rp2xxx.h"

namespace reg = kickos::rp2040::reg;
namespace irq = kickos::rp2040::irq;

using kickos::rp2xxx::ATOMIC_CLR;
using kickos::rp2xxx::ATOMIC_SET;
using kickos::rp2xxx::unreset;

extern "C"
{
    void kickos_armv6m_init(void);

    // SysTick reads this live; clocks_init updates it with every clk_sys switch.
    uint32_t SystemCoreClock = 12000000u;
}

namespace
{
    constexpr uintptr_t led_out(bool level)
    {
        if (level)
        {
            return reg::sio::GPIO_OUT_SET;
        }
        return reg::sio::GPIO_OUT_CLR;
    }

    constexpr bool LED_LIT = KICKOS_BOARD_LED_ACTIVE_LOW == 0;
}

extern "C"
{

void arch_init(void)
{
    // Reset-release ordering is load-bearing: a RESET_DONE only asserts once the peripheral has a
    // running clock. IO_BANK0/PADS_BANK0/TIMER run off clk_sys/clk_ref, live off the ROSC at
    // reset; UART0 runs off clk_peri, OFF until clocks_init, so releasing it BEFORE that hangs the
    // boot.
    unreset(reg::resets::IO_BANK0 | reg::resets::PADS_BANK0 | reg::resets::TIMER);
    kickos::rp2xxx::clocks_init();
    unreset(reg::resets::UART0);
    kickos::rp2xxx::console_uart_init();
    if (kickos_usb_device_console != 0)
    {
        kickos::rp2xxx::usb_clock_init();
    }
    kickos_armv6m_init();
}

console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
    *irq_line = irq::UART0_IRQ;
    return &kickos::rp2xxx::console_backend;
}

void arch_diag_led_init(void)
{
    kickos::rp2xxx::pin_function(KICKOS_BOARD_LED_BIT, reg::io_bank0::FUNCSEL_SIO
                                                           | kickos::rp2xxx::PIN_DRIVE
                                                           | kickos::rp2xxx::PIN_SIO_OUT);
}

void arch_diag_led_set(int on)
{
    if (on)
    {
        r32(led_out(LED_LIT)) = 1u << KICKOS_BOARD_LED_BIT;
    }
    else
    {
        r32(led_out(not LED_LIT)) = 1u << KICKOS_BOARD_LED_BIT;
    }
}

// A reset is the watchdog's (DS 4.7), with SCRATCH4 cleared or the bootrom takes a stale magic
// there for a vector to jump to. The bootloader is the bootrom's USB download mode
// (_reset_to_usb_boot, DS 2.8.3). The three magic bytes at 0x10 are the whole validity test the
// datasheet gives; the byte at 0x13 is a ROM build number it forbids using to locate functions.
int arch_reboot(uint32_t outcome)
{
    if (outcome == KOS_SHUTDOWN_REBOOT)
    {
        __asm volatile("cpsid i" ::: "memory");
        r32(reg::watchdog::CTRL + ATOMIC_CLR) = reg::watchdog::CTRL_ENABLE;
        r32(reg::watchdog::SCRATCH4) = 0u;
        r32(reg::psm::WDSEL) = reg::psm::WDSEL_ALL_BUT_OSCILLATORS;
        r32(reg::watchdog::CTRL + ATOMIC_SET) = reg::watchdog::CTRL_TRIGGER;
        kickos::arch::reset_settle();
        kickos::arch::reboot_failed(kickos::diag::kResetNotTaken);
    }
    if (outcome != KOS_SHUTDOWN_BOOTLOADER)
    {
        return -KOS_ENOSYS;
    }
    // GCC assumes the first bytes of the address space are unmapped; 0x0 is the bootrom.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    if (r8(0x10u) != 'M' or r8(0x11u) != 'u' or r8(0x12u) != 0x01u)
    {
        return -KOS_ENOSYS;
    }
    // The halfword pointers already carry the Thumb bit: the table, then the lookup helper.
    uint32_t const table = r16(0x14u);
    uintptr_t const lookup = r16(0x18u);
#pragma GCC diagnostic pop

    // 'UB' packs as (c2 << 8) | c1.
    kickos_rp_rom_args const find = {lookup, table, ('B' << 8) | 'U', 0u, 0u};
    uintptr_t const usb_boot = kickos_rp_rom_call(&find);
    if (usb_boot == 0u)
    {
        return -KOS_ENOSYS;
    }

    __asm volatile("cpsid i" ::: "memory"); // dispatch runs in thread mode with IRQs live
    // (0, 0): no activity LED, neither USB interface disabled.
    kickos_rp_rom_args const boot = {usb_boot, 0u, 0u, 0u, 0u};
    (void)kickos_rp_rom_call(&boot);
    kickos::arch::reboot_failed(kickos::diag::kRebootRp2040);
}

void Reset_Handler(void)
{
    kickos_ranges_init();
    kickos_crt_tail();
}

}
