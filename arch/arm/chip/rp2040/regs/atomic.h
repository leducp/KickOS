// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 atomic register-access aliases (RP2040 datasheet, RP-008371-DS, 2.1.2).
// Every APB peripheral's register block is mirrored three times above its normal
// window: a write to base+0x1000 XORs, base+0x2000 SETs, base+0x3000 CLEARs the
// addressed register by the written bitmask -- so a single-bit change needs no
// read-modify-write. This applies to the APB blocks in mmap.h (CLOCKS, RESETS,
// IO_BANK0, PADS_BANK0, XOSC, PLL_SYS, UART0, ...). It does NOT apply to SIO
// (regs/sio.h, its own SET/CLR/XOR registers) nor to the XIP SSI.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_ATOMIC_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_ATOMIC_H

#include <stdint.h>

namespace kickos::rp2040::reg::atomic
{
    constexpr uintptr_t XOR_ALIAS = 0x1000u;
    constexpr uintptr_t SET_ALIAS = 0x2000u;
    constexpr uintptr_t CLR_ALIAS = 0x3000u;

    // A write of `mask` to these addresses XORs / SETs / CLEARs the register at
    // `addr` by that bitmask (addr = the register's normal-window address).
    constexpr uintptr_t as_xor(uintptr_t addr) { return addr + XOR_ALIAS; }
    constexpr uintptr_t as_set(uintptr_t addr) { return addr + SET_ALIAS; }
    constexpr uintptr_t as_clr(uintptr_t addr) { return addr + CLR_ALIAS; }
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_ATOMIC_H
