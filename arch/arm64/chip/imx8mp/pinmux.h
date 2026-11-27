// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The i.MX 8M Plus pads KOS_SYS_PINMUX_SET reaches: GPIO5's bits 22 to 29, the pads UART1_RXD to
// UART4_TXD, and the registers one call may write for one of them (IMX8MPRM rev 3, 8.2.4).
//
// func encoding:
//   bits[4:0]   SW_MUX_CTL_PAD: MUX_MODE[2:0] | SION (bit 4)
//   bit[5]      write SW_PAD_CTL_PAD
//   bits[14:6]  its value: DSE[2:1], FSEL, ODE, PUE, HYS, PE
//   bit[15]     write the RXD input select of the pad's own UART, on a RXD pad alone
//   bits[19:16] its DAISY value, within the width of that UART's DAISY field
// Every other bit is refused, as is a bit no register field takes.

#ifndef KICKOS_ARCH_ARM64_CHIP_IMX8MP_PINMUX_H
#define KICKOS_ARCH_ARM64_CHIP_IMX8MP_PINMUX_H

#include <kickos/chip_mmap.h>
#include <kickos/sys/errno.h>

#include <stdint.h>

namespace kickos::imx8mp::pinmux
{
    constexpr uint32_t PORT = 5u;
    constexpr uint32_t FIRST_BIT = 22u;
    constexpr uint32_t LAST_BIT = 29u;

    constexpr uint32_t MUX_MASK = 0x17u;
    constexpr uint32_t PAD_WRITE = 1u << 5;
    constexpr uint32_t PAD_SHIFT = 6u;
    constexpr uint32_t PAD_MASK = 0x1F6u;
    constexpr uint32_t DAISY_WRITE = 1u << 15;
    constexpr uint32_t DAISY_SHIFT = 16u;
    constexpr uint32_t DAISY_MASK = 0xFu;
    constexpr uint32_t FUNC_FIELDS = MUX_MASK | PAD_WRITE | (0x1FFu << PAD_SHIFT) | DAISY_WRITE
                                     | (DAISY_MASK << DAISY_SHIFT);

    // SW_MUX_CTL_PAD and SW_PAD_CTL_PAD of GPIO5's pad `bit`, one word each from 0x220 and from
    // 0x480 (8.2.4.132 to 8.2.4.139, 8.2.4.284 to 8.2.4.291).
    constexpr uintptr_t sw_mux(uint32_t bit)
    {
        return mmap::IOMUXC_BASE + 0x220u + (bit - FIRST_BIT) * 4u;
    }
    constexpr uintptr_t sw_pad(uint32_t bit)
    {
        return mmap::IOMUXC_BASE + 0x480u + (bit - FIRST_BIT) * 4u;
    }

    // IOMUXC_UARTn_UART_RXD_MUX_SELECT_INPUT of the UART whose RXD pad is `bit`, one per UART every
    // 8 bytes from 0x5E8 (8.2.4.374 to 8.2.4.380); 0 for a TXD pad, which has none.
    constexpr uintptr_t rxd_select(uint32_t bit)
    {
        if (((bit - FIRST_BIT) & 1u) != 0u)
        {
            return 0;
        }
        return mmap::IOMUXC_BASE + 0x5E8u + ((bit - FIRST_BIT) / 2u) * 8u;
    }

    // The width of that DAISY field: [2:0] for UART1 to UART3, [3:0] for UART4 (8.2.4.374 to
    // 8.2.4.380).
    constexpr uint32_t rxd_select_bits(uint32_t bit)
    {
        if ((bit - FIRST_BIT) / 2u == 3u)
        {
            return 4u;
        }
        return 3u;
    }

    // The writes one call makes: rc 0 and each register to write with its value, or a refusal and
    // no write.
    struct Plan
    {
        int rc;
        uintptr_t mux;
        uint32_t mux_value;
        uintptr_t pad;
        uint32_t pad_value;
        uintptr_t daisy;
        uint32_t daisy_value;
    };

    // `listed` and `kernel_owned` answer for the pad (port, bit): the chip file lists it, and the
    // board's kernel holds it, the console's pads among them.
    constexpr Plan plan(uint32_t port, uint32_t bit, uint32_t func, bool listed, bool kernel_owned)
    {
        Plan p = {-KOS_EINVAL, 0, 0, 0, 0, 0, 0};
        if (port != PORT or bit < FIRST_BIT or bit > LAST_BIT or not listed)
        {
            return p;
        }
        if (kernel_owned)
        {
            p.rc = -KOS_EBUSY;
            return p;
        }
        if ((func & ~FUNC_FIELDS) != 0u)
        {
            return p;
        }
        uint32_t const pad_value = (func >> PAD_SHIFT) & 0x1FFu;
        if ((pad_value & ~PAD_MASK) != 0u)
        {
            return p;
        }
        if ((func & DAISY_WRITE) != 0u
            and (rxd_select(bit) == 0u or ((func >> DAISY_SHIFT) & DAISY_MASK) >> rxd_select_bits(bit) != 0u))
        {
            return p;
        }
        p.rc = 0;
        p.mux = sw_mux(bit);
        p.mux_value = func & MUX_MASK;
        if ((func & PAD_WRITE) != 0u)
        {
            p.pad = sw_pad(bit);
            p.pad_value = pad_value;
        }
        if ((func & DAISY_WRITE) != 0u)
        {
            p.daisy = rxd_select(bit);
            p.daisy_value = (func >> DAISY_SHIFT) & DAISY_MASK;
        }
        return p;
    }
}

#endif
