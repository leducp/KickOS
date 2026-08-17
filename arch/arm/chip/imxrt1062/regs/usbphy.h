// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 USB1 clock-tree and PHY registers, the KERNEL's half of the USB device
// console (RM ch.14 and ch.43). The controller's own register map is the unprivileged
// driver's and lives in system/driver/imxrt1062/.
//
// RM Table 9-6 "List of Disabled Clock Gate Enables" names CCM_CCGR6_CG0, the usboh3 gate:
// the boot ROM hands the USB controller over UNCLOCKED, and every register in the driver's
// window reads back as nothing until CCGR6 is written.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_USBPHY_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_USBPHY_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::imxrt1062::reg::usbphy
{
    // CCM_ANALOG PLL_USB1, 480 MHz (RM 14.8.2, base + 0x10 with the SET/CLR/TOG aliases
    // at +4/+8/+0xC).
    constexpr uintptr_t PLL_USB1 = mmap::CCM_ANALOG_BASE + 0x10u;
    constexpr uintptr_t PLL_USB1_SET = PLL_USB1 + 0x04u;
    constexpr uintptr_t PLL_USB1_CLR = PLL_USB1 + 0x08u;

    constexpr uint32_t PLL_LOCK = 1u << 31;
    constexpr uint32_t PLL_BYPASS = 1u << 16;
    constexpr uint32_t PLL_ENABLE = 1u << 13;
    constexpr uint32_t PLL_POWER = 1u << 12;
    constexpr uint32_t PLL_EN_USB_CLKS = 1u << 6; // the 9-phase outputs USBPHY1 needs
    constexpr uint32_t PLL_DIV_SELECT_22 = 1u << 1; // 0 = Fref * 20 = 480 MHz

    // RM Table 9-7 "ROM Clock Setting": the boot ROM leaves PLL_USB1 at 0x8000_3040, which is
    // LOCK | ENABLE | POWER | EN_USB_CLKS.
    constexpr uint32_t PLL_ROM_SETTING = 0x80003040u;

    // USBPHY1 (RM 43.4). Every register has SET/CLR/TOG aliases at +4/+8/+0xC.
    constexpr uintptr_t PHY1_PWD = mmap::USBPHY1_BASE + 0x00u;
    constexpr uintptr_t PHY1_CTRL = mmap::USBPHY1_BASE + 0x30u;
    constexpr uintptr_t PHY1_CTRL_SET = PHY1_CTRL + 0x04u;
    constexpr uintptr_t PHY1_CTRL_CLR = PHY1_CTRL + 0x08u;

    // USBPHY1_CTRL (RM 43.4.4). Resets to 0xC020_0000, so SFTRST and CLKGATE are BOTH set
    // and the PHY is held in reset with its UTMI clocks gated.
    constexpr uint32_t CTRL_SFTRST = 1u << 31;
    constexpr uint32_t CTRL_CLKGATE = 1u << 30;
    constexpr uint32_t CTRL_ENUTMILEVEL2 = 1u << 14;
    constexpr uint32_t CTRL_ENUTMILEVEL3 = 1u << 15;

    // USBPHY1_PWD (RM 43.4.1) resets to 0x001E_1C00: every analog block powered DOWN, so zero
    // is the whole of "powered up". RM 43.4.1 requires the PHY clocks to be running first.
    constexpr uint32_t PWD_ALL_UP = 0u;

    // USBNC OTG1 control (RM 42.6.1). Non-core, so outside the driver's window. Reset
    // 0x3000_1000 carries bits the field table calls reserved and RM 42.6 says to preserve a
    // reserved bit's value on write, so writes here are read-modify-write, never absolute.
    constexpr uintptr_t USBNC_OTG1_CTRL = mmap::USBNC_BASE + 0x00u;
    constexpr uint32_t OTG_CTRL_OVER_CUR_DIS = 1u << 7;
    constexpr uint32_t OTG_CTRL_OVER_CUR_POL = 1u << 8;
    constexpr uint32_t OTG_CTRL_PWR_POL = 1u << 9;

    // CCM_CCGR6 CG0 = usboh3 (RM 14.7.27). Both bits of the field, run mode plus wait.
    constexpr uintptr_t CCGR6 = mmap::CCM_BASE + 0x80u;
    constexpr uint32_t CCGR6_USBOH3 = 3u << 0;
}

#endif
