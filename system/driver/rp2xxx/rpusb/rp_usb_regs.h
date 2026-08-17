// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 / RP2350 USB device controller register map. ONE header for both parts: the two
// blocks are the same IP and the RP2350 is a documented superset. Citations are
// "RP2350 DS <sec> / RP2040 DS <sec>" where the two documents number the same fact
// differently. Offsets only, no code; hand-rolled from the datasheets, no vendor SDK.
// RESETS, CLOCKS and PLL_USB are the kernel's and stay in arch/arm/chip/<chip>/.
//
// TWO ACCESS RULES that are not visible in the offsets and will corrupt a transfer if
// forgotten:
//   - The DPRAM has NO set/clear/xor aliases (RP2350 DS 12.7.3.7 / RP2040 DS 4.1.2.7).
//     Only the register block at +0x10000 has them.
//   - A buffer control word must be written in TWO steps: the length/PID/FULL bits
//     first, then AVAILABLE after at least one clk_usb cycle has passed (RP2350 DS
//     12.7.3.7.1 / RP2040 DS 4.1.2.7.1). The RP2350 has a double-read fix that makes
//     this unnecessary, the RP2040 does not.

#ifndef KICKOS_DRIVER_RP2XXX_RP_USB_REGS_H
#define KICKOS_DRIVER_RP2XXX_RP_USB_REGS_H

#include <stdint.h>

namespace kickos::rpusb::reg
{
    // RP2350 DS 2.2.5 / RP2040 DS 2.2.2. Identical on both parts.
    constexpr uintptr_t DPRAM_BASE = 0x50100000u;
    constexpr uintptr_t REGS_BASE = 0x50110000u;
    constexpr uint32_t DPRAM_SIZE = 4096u;

    // DPRAM layout (RP2350 DS 12.7.3.7.2 / RP2040 DS 4.1.2.7.2).
    constexpr uintptr_t DP_SETUP = 0x000u; // 8 bytes, hardware-written
    // Endpoint control, EP1..EP15 only: EP0 has none, its interrupt enables come from
    // SIE_CTRL (RP2350 DS 12.7.3.7.3).
    constexpr uintptr_t dp_ep_ctrl_in(uint8_t ep) { return 0x00u + 8u * ep; }
    constexpr uintptr_t dp_ep_ctrl_out(uint8_t ep) { return 0x04u + 8u * ep; }
    // Buffer control, EP0..EP15: EP0 IS present here.
    constexpr uintptr_t dp_buf_ctrl_in(uint8_t ep) { return 0x80u + 8u * ep; }
    constexpr uintptr_t dp_buf_ctrl_out(uint8_t ep) { return 0x84u + 8u * ep; }
    // EP0's single 64-byte buffer, shared between IN and OUT.
    constexpr uintptr_t DP_EP0_BUF = 0x100u;
    // First general data buffer. Buffer addresses are 64-byte aligned because the
    // endpoint control word carries only bits 15:6 of the offset.
    constexpr uintptr_t DP_DATA_BASE = 0x180u;

    // Endpoint control word (RP2350 DS 12.7.3.7.3 Table 1193).
    constexpr uint32_t EPC_ENABLE = 1u << 31;
    constexpr uint32_t EPC_DOUBLE_BUF = 1u << 30;
    constexpr uint32_t EPC_INT_1BUF = 1u << 29;
    constexpr uint32_t EPC_INT_2BUF = 1u << 28;
    constexpr uint32_t EPC_TYPE_SHIFT = 26; // 0 control, 1 iso, 2 bulk, 3 interrupt
    constexpr uint32_t EPC_INT_STALL = 1u << 17;
    constexpr uint32_t EPC_INT_NAK = 1u << 16;
    constexpr uint32_t EPC_ADDR_MASK = 0xFFC0u; // bits 15:6, the buffer offset in DPRAM

    // Buffer control word, buffer 0 half (RP2350 DS 12.7.3.7.4 Table 1194). The upper
    // half is buffer 1 and is unused here: every endpoint is single-buffered.
    constexpr uint32_t BUF_FULL = 1u << 15;      // CPU sets for IN, clears for OUT
    constexpr uint32_t BUF_LAST = 1u << 14;
    constexpr uint32_t BUF_PID_DATA1 = 1u << 13; // software-owned: the SIE never toggles it
    constexpr uint32_t BUF_RESET_SEL = 1u << 12;
    constexpr uint32_t BUF_STALL = 1u << 11;
    constexpr uint32_t BUF_AVAILABLE = 1u << 10;
    constexpr uint32_t BUF_LEN_MASK = 0x3FFu;

    // Register block, REGS_BASE-relative (RP2350 DS 12.7.5 Table 1195).
    constexpr uintptr_t ADDR_ENDP = 0x000u;
    constexpr uintptr_t MAIN_CTRL = 0x040u;
    constexpr uintptr_t SIE_CTRL = 0x04Cu;
    constexpr uintptr_t SIE_STATUS = 0x050u;
    constexpr uintptr_t BUFF_STATUS = 0x058u;
    constexpr uintptr_t EP_ABORT = 0x060u;
    constexpr uintptr_t EP_STALL_ARM = 0x068u;
    constexpr uintptr_t USB_MUXING = 0x074u;
    constexpr uintptr_t USB_PWR = 0x078u;
    constexpr uintptr_t INTR = 0x08Cu;
    constexpr uintptr_t INTE = 0x090u;
    constexpr uintptr_t INTS = 0x098u;

    // MAIN_CTRL. PHY_ISO is RP2350-only and resets SET; on the RP2040 bit 2 is reserved
    // and reads zero, so ONE absolute write of CONTROLLER_EN alone satisfies the RP2350's
    // single mandatory delta (RP2350 DS 12.7.2) on both parts. A read-modify-write or a
    // set-alias write would leave the PHY isolated.
    constexpr uint32_t MAIN_CTRL_CONTROLLER_EN = 1u << 0;
    constexpr uint32_t MAIN_CTRL_HOST_NDEVICE = 1u << 1;
    constexpr uint32_t MAIN_CTRL_PHY_ISO = 1u << 2;

    // SIE_CTRL. PULLDOWN_EN resets SET on the RP2350 and clear on the RP2040, and a device
    // must not pull down, so every write to SIE_CTRL is absolute.
    constexpr uint32_t SIE_CTRL_EP0_INT_1BUF = 1u << 29;
    constexpr uint32_t SIE_CTRL_RESUME = 1u << 12;
    constexpr uint32_t SIE_CTRL_PULLUP_EN = 1u << 16;
    constexpr uint32_t SIE_CTRL_PULLDOWN_EN = 1u << 15;

    // SIE_STATUS, write-1-to-clear except the read-only status bits.
    constexpr uint32_t SIE_STATUS_DATA_SEQ_ERROR = 1u << 31;
    constexpr uint32_t SIE_STATUS_RX_OVERFLOW = 1u << 26;
    constexpr uint32_t SIE_STATUS_BIT_STUFF_ERROR = 1u << 25;
    constexpr uint32_t SIE_STATUS_CRC_ERROR = 1u << 24;
    constexpr uint32_t SIE_STATUS_BUS_RESET = 1u << 19;
    constexpr uint32_t SIE_STATUS_TRANS_COMPLETE = 1u << 18;
    constexpr uint32_t SIE_STATUS_SETUP_REC = 1u << 17;
    constexpr uint32_t SIE_STATUS_CONNECTED = 1u << 16;
    constexpr uint32_t SIE_STATUS_RESUME = 1u << 11;
    constexpr uint32_t SIE_STATUS_SUSPENDED = 1u << 4;

    // USB_MUXING: connect the controller to the on-chip PHY. TO_PHY resets SET on the
    // RP2350 and clear on the RP2040, so this too is written absolutely.
    constexpr uint32_t USB_MUXING_TO_PHY = 1u << 0;
    constexpr uint32_t USB_MUXING_SOFTCON = 1u << 3;

    // USB_PWR: force VBUS-detected. Write the value, then the override enable.
    constexpr uint32_t USB_PWR_VBUS_DETECT = 1u << 2;
    constexpr uint32_t USB_PWR_VBUS_DETECT_OVERRIDE_EN = 1u << 3;

    // INTR / INTE / INTS share one layout (RP2350 DS 12.7.5 Tables 1217-1220). Every bit
    // used here is cleared AT ITS SOURCE, not by writing the interrupt register, which is
    // why the line is claimed KOS_IRQ_LEVEL.
    constexpr uint32_t INT_SETUP_REQ = 1u << 16;  // mirrors SIE_STATUS.SETUP_REC (bit 17)
    constexpr uint32_t INT_DEV_SUSPEND = 1u << 14;
    constexpr uint32_t INT_DEV_CONN_DIS = 1u << 13;
    constexpr uint32_t INT_BUS_RESET = 1u << 12;
    constexpr uint32_t INT_BUFF_STATUS = 1u << 4; // stays asserted until BUFF_STATUS is 0

    // BUFF_STATUS / EP_STALL_ARM bit positions: EPn IN is bit 2n, EPn OUT is bit 2n+1.
    // BUFF_STATUS is write-1-to-clear; EP_STALL_ARM has only its two EP0 bits.
    constexpr uint32_t buff_bit_in(uint8_t ep) { return 1u << (2u * ep); }
    constexpr uint32_t buff_bit_out(uint8_t ep) { return 1u << (2u * ep + 1u); }
    constexpr uint32_t STALL_ARM_EP0_IN = 1u << 0;
    constexpr uint32_t STALL_ARM_EP0_OUT = 1u << 1;
}

#endif
