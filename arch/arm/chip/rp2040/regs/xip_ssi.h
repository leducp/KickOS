// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 XIP SSI register map (RP2040 datasheet, RP-008371-DS, 4.10.13): the
// Synopsys SSI that fronts the QSPI flash for execute-in-place. Configured once by
// the second-stage bootloader (boot2.S) for the "03h serial read per access" XIP
// mode; the SSI cannot be reconfigured while enabled (4.10.9). Not on the APB, so
// no atomic alias window. These duplicate the .equ constants in boot2.S (which,
// being assembly, cannot include this header) for reference/inventory.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_XIP_SSI_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_XIP_SSI_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2040::reg::xip_ssi
{
    constexpr uintptr_t CTRLR0 = mmap::XIP_SSI_BASE + 0x00u;
    constexpr uintptr_t CTRLR1 = mmap::XIP_SSI_BASE + 0x04u;
    constexpr uintptr_t SSIENR = mmap::XIP_SSI_BASE + 0x08u; // SSI enable (bit 0)
    constexpr uintptr_t BAUDR = mmap::XIP_SSI_BASE + 0x14u;  // SCK = clk_sys / BAUDR
    constexpr uintptr_t SPI_CTRLR0 = mmap::XIP_SSI_BASE + 0xf4u;

    // CTRLR0 = 0x001f0300: SPI_FRF=STD(0), DFS_32=31 (32-bit frames),
    // TMOD=EEPROM_READ(3): transmit the cmd/addr control frame, then receive.
    constexpr uint32_t CTRLR0_XIP = 0x001f0300u;
    // SPI_CTRLR0 = 0x03000218: XIP_CMD=0x03, INST_L=8b(2<<8), ADDR_L=24b(6<<2),
    // TRANS_TYPE=1C1A(0): command + address both in standard 1-bit SPI.
    constexpr uint32_t SPI_CTRLR0_XIP = 0x03000218u;
    // /4 keeps SCK within any flash's 03h read limit at the boot clk_sys.
    constexpr uint32_t BAUDR_DIV = 4u;

    // XIP-mapped flash: the boot2 payload occupies the first 256 bytes; the
    // application vector table follows immediately after.
    constexpr uintptr_t XIP_WINDOW = 0x10000000u;
    constexpr uintptr_t APP_VECTORS = 0x10000100u;
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_XIP_SSI_H
