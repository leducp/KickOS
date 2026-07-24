// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 GPIO port registers + IOCR PC-field encoding (RM ch.26). Clean-room
// from the XMC4700/XMC4800 Reference Manual (V1.3, 2016-07). The PC codes and
// the shift/mask helpers are SHARED by the pinmux and GPIO backends and the
// console pin setup, so a single definition keeps their views identical.

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_REGS_PORT_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_REGS_PORT_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::xmc::reg::port
{
    constexpr uintptr_t base(uint32_t port) { return mmap::PORT0_BASE + port * mmap::PORT_STRIDE; }

    namespace off
    {
        constexpr uintptr_t OMR = 0x04;    // Output Modification (set/reset in one write)
        constexpr uintptr_t IOCR0 = 0x10;  // IOCRx group = IOCR0 + (pin/4)*4 (IOCR0/4/8/12)
        constexpr uintptr_t IN = 0x24;     // RM 26.8.6: read-only pad input (readable even as PP output)
    }

    // The IOCR register controlling `pin` and the PC-field position within it.
    // Each IOCRx holds four 5-bit PC fields; field n is at bit (n*8 + 3).
    constexpr uintptr_t iocr_off(uint32_t pin) { return off::IOCR0 + (pin / 4u) * 4u; }
    constexpr uint32_t pc_shift(uint32_t pin) { return (pin % 4u) * 8u + 3u; }
    constexpr uint32_t PC_FIELD_MASK = 0x1Fu; // PC is 5 bits

    // PC codes (RM Table 26-5 "Port I/O Control").
    constexpr uint32_t PC_INPUT_NOPULL = 0x00u; // input, direct, no internal pull device
    constexpr uint32_t PC_OUTPUT_PP_GP = 0x10u; // output push-pull, general purpose
    constexpr uint32_t PC_PP_ALT2 = 0x12u;      // output push-pull, alternate function 2

    // Class boundary used by the GPIO backend: a PC code < this is an input
    // configuration, >= it is an output configuration.
    constexpr uint32_t PC_INPUT_CLASS_MAX = 0x08u;

    // OMR: write 1<<pin to drive high, 1<<(pin+OMR_RESET_SHIFT) to drive low.
    constexpr uint32_t OMR_RESET_SHIFT = 16u;
}

#endif
