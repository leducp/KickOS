// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M peripheral BASE addresses. Transcribed from the RX72M Group User's Manual:
// Hardware (r01uh0804ej0120, Rev.1.20); hand-rolled, clean-room (no vendor SDK).
// Register offsets + fields live in regs/<periph>.h.
//
// The ICU/CMTW/MPU register FILES are defined at the arch layer in arch/rx/rxv3/regs.h
// (kickos::rxv3); the bases here are the ones the chip backend references directly
// (arch_reserved_blocks, the CMTW/ICU wiring).

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_MMAP_H
#define KICKOS_ARCH_RX_CHIP_RX72M_MMAP_H

#include <stdint.h>

namespace kickos::rx::mmap
{
    // SYSTEM / low-power / clock-generation register block (UM sec.9/11/13).
    // NOTE: MOFCR (main osc forced oscillation, sec.9.2.19) sits OUTSIDE this
    // block at 0x0008C293 (see regs/cgc.h).
    constexpr uintptr_t SYSTEM = 0x00080000;
    // Flash ROM cache control (UM sec.64.4). Not in the PRCR protect list.
    constexpr uintptr_t FLASH = 0x00081000;
    // Memory-Protection Unit register file (UM sec.17). Register fields are in
    // arch/rx/rxv3/regs.h; the chip only reserves this block for the kernel.
    constexpr uintptr_t MPU = 0x00086400;
    // ICUD interrupt controller (UM sec.15). IR/IER/IPR bases in rxv3/regs.h.
    constexpr uintptr_t ICU = 0x00087000;
    // SCI6, the board console UART (UM sec.42).
    constexpr uintptr_t SCI6 = 0x0008A0C0;
    // Compare Match Timer W (UM sec.32). Register fields in rxv3/regs.h.
    constexpr uintptr_t CMTW0 = 0x00094200; // one-shot next-event timer
    constexpr uintptr_t CMTW1 = 0x00094280; // free-running monotonic clock
    // I/O ports register block (UM sec.22): PDR/PODR/PMR blocks at fixed offsets.
    constexpr uintptr_t PORT = 0x0008C000;
    // Multi-function pin controller (UM sec.23): PWPR + per-pin PFS.
    constexpr uintptr_t MPC = 0x0008C100;
}

#endif
