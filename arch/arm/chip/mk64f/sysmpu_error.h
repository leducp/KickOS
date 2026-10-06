// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The SYSMPU's error latches (K64 RM 19.3): CESR's SPERR field, bit 31 naming slave port 0, and
// per port an EAR and an EDR whose EMN field [7:4] names the logical bus master.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_SYSMPU_ERROR_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_SYSMPU_ERROR_H

#include <stddef.h>
#include <stdint.h>

// Offsets from the SYSMPU base, answered by the chip and by the host gate's planted latches.
extern "C" uint32_t sysmpu_error_read(uintptr_t offset);
extern "C" void sysmpu_error_write(uintptr_t offset, uint32_t value);

constexpr uintptr_t SYSMPU_ERROR_CESR = 0x000u;
constexpr uint32_t SYSMPU_ERROR_CESR_VLD = 1u << 0;
constexpr size_t SYSMPU_ERROR_PORTS = 5;
// RM Table 3-20: logical master 0 is the core; 1 is the debugger, then the DMA engines.
constexpr uint32_t SYSMPU_ERROR_CORE_MASTER = 0u;

// The lowest slave port whose protection error is latched, read and then cleared. SPERR is W1C
// and VLD plain R/W, so VLD is written back as read: a bare write of the port's bit would
// disable the whole SYSMPU.
static inline bool sysmpu_take_error(size_t* port, uint32_t* ear, uint32_t* edr)
{
    uint32_t const cesr = sysmpu_error_read(SYSMPU_ERROR_CESR);
    for (size_t p = 0; p < SYSMPU_ERROR_PORTS; p++)
    {
        uint32_t const bit = 1u << (31u - p);
        if ((cesr & bit) == 0u)
        {
            continue;
        }
        *port = p;
        *ear = sysmpu_error_read(0x010u + p * 8u);
        *edr = sysmpu_error_read(0x014u + p * 8u);
        sysmpu_error_write(SYSMPU_ERROR_CESR, (cesr & SYSMPU_ERROR_CESR_VLD) | bit);
        return true;
    }
    return false;
}

// Clears every latch the read finds, and answers the address of the lowest port's error the
// core's bus master made: another master's error explains no core fault.
static inline bool sysmpu_core_error_addr(uintptr_t* addr)
{
    bool found = false;
    size_t port = 0;
    uint32_t ear = 0;
    uint32_t edr = 0;
    for (size_t n = 0; n < SYSMPU_ERROR_PORTS and sysmpu_take_error(&port, &ear, &edr); n++)
    {
        if (not found and ((edr >> 4) & 0xFu) == SYSMPU_ERROR_CORE_MASTER)
        {
            *addr = ear;
            found = true;
        }
    }
    return found;
}

#endif
