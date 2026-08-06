// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M flash ROM-cache register offsets + fields. From the RX72M Group User's
// Manual: Hardware (r01uh0804ej0120, Rev.1.20) sec.64.4.1/64.4.2; hand-rolled,
// clean-room. 16-bit access; not in the PRCR Table 13.1 protect list, so no
// unlock is needed. Bases: mmap.h.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_REGS_FLASH_H
#define KICKOS_ARCH_RX_CHIP_RX72M_REGS_FLASH_H

#include <stdint.h>

#include <kickos/chip_mmap.h>

namespace kickos::rx::reg::flash
{
    constexpr uintptr_t ROMCE = mmap::FLASH + 0x000;  // ROM cache enable (sec.64.4.1)
    constexpr uintptr_t ROMCIV = mmap::FLASH + 0x004; // ROM cache invalidate (sec.64.4.2)
    constexpr uint16_t ROMCE_ROMCEN = 1u << 0;        // enable ROM cache
    constexpr uint16_t ROMCIV_ROMCIV = 1u << 0;       // write 1 = start; reads 1 while in progress
}

#endif
