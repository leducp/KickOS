// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bases the chips' <regs/...> headers name, for every chip this suite brings up: a host build
// generates no chip header for these parts.

#ifndef KICKOS_TESTS_UNIT_CLOCKBRINGUP_SEAM_KICKOS_CHIP_MMAP_H
#define KICKOS_TESTS_UNIT_CLOCKBRINGUP_SEAM_KICKOS_CHIP_MMAP_H

#include <stdint.h>

namespace kickos::stm32f411::mmap
{
    constexpr uintptr_t RCC_BASE = 0x40023800u;
    constexpr uintptr_t FLASH_BASE = 0x40023C00u;
}

namespace kickos::mk64f::mmap
{
    constexpr uintptr_t SIM_BASE = 0x40047000u;
    constexpr uintptr_t MCG_BASE = 0x40064000u;
    constexpr uintptr_t OSC0_BASE = 0x40065000u;
}

namespace kickos::sam3x8e::mmap
{
    constexpr uintptr_t EEFC0_BASE = 0x400E0A00u;
    constexpr uintptr_t EEFC1_BASE = 0x400E0C00u;
    constexpr uintptr_t PMC_BASE = 0x400E0600u;
}

namespace kickos::xmc::mmap
{
    constexpr uintptr_t SCU_TRAP_BASE = 0x50004160u;
    constexpr uintptr_t SCU_RCU_BASE = 0x50004400u;
    constexpr uintptr_t SCU_CLK_BASE = 0x50004600u;
    constexpr uintptr_t SCU_OSC_BASE = 0x50004700u;
    constexpr uintptr_t SCU_PLL_BASE = 0x50004710u;
    constexpr uintptr_t FLASH0_CTRL_BASE = 0x58001000u;
    constexpr uintptr_t USIC0_CH0_BASE = 0x40030000u;
    constexpr uintptr_t USIC0_CH1_BASE = 0x40030200u;
}

namespace kickos::rx::mmap
{
    constexpr uintptr_t SYSTEM = 0x00080000u;
    constexpr uintptr_t SYSTEM_LP_BASE = 0x0008C280u;
    constexpr uintptr_t SCI6 = 0x0008A0C0u;
}

namespace kickos::esp32c6::mmap
{
    constexpr uintptr_t TIMG0_BASE = 0x60008000u;
    constexpr uintptr_t PCR_BASE = 0x60096000u;
    constexpr uintptr_t CLINT_BASE = 0x20001800u;
    constexpr uintptr_t SPI0_BASE = 0x60002000u;
    constexpr uintptr_t EXTMEM_BASE = 0x600C8000u;
}

namespace kickos::rp2040::mmap
{
    constexpr uintptr_t CLOCKS_BASE = 0x40008000u;
    constexpr uintptr_t RESETS_BASE = 0x4000C000u;
    constexpr uintptr_t PSM_BASE = 0x40010000u;
    constexpr uintptr_t XOSC_BASE = 0x40024000u;
    constexpr uintptr_t PLL_SYS_BASE = 0x40028000u;
    constexpr uintptr_t PLL_USB_BASE = 0x4002C000u;
    constexpr uintptr_t UART0_BASE = 0x40034000u;
    constexpr uintptr_t WATCHDOG_BASE = 0x40058000u;
}

namespace kickos::rp2350::mmap
{
    constexpr uintptr_t CLOCKS_BASE = 0x40010000u;
    constexpr uintptr_t RESETS_BASE = 0x40020000u;
    constexpr uintptr_t PADS_BANK0_BASE = 0x40038000u;
    constexpr uintptr_t XOSC_BASE = 0x40048000u;
    constexpr uintptr_t PLL_SYS_BASE = 0x40050000u;
    constexpr uintptr_t PLL_USB_BASE = 0x40058000u;
    constexpr uintptr_t UART1_BASE = 0x40078000u;
    constexpr uintptr_t TICKS_BASE = 0x40108000u;
}

#endif
