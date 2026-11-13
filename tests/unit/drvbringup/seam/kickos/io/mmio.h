// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Stands in for system/include's: every MMIO access is a call into the test's register model,
// so a write-1-to-clear register clears, and a case can act between two accesses.

#ifndef KICKOS_TESTS_UNIT_DRVBRINGUP_SEAM_KICKOS_IO_MMIO_H
#define KICKOS_TESTS_UNIT_DRVBRINGUP_SEAM_KICKOS_IO_MMIO_H

#include <stdint.h>

uint32_t mmio_read(uintptr_t addr, uint32_t width);
void mmio_write(uintptr_t addr, uint32_t width, uint32_t value);

template <typename T>
struct MmioReg
{
    uintptr_t addr;

    operator T() const { return static_cast<T>(mmio_read(addr, sizeof(T))); }

    MmioReg& operator=(T value)
    {
        mmio_write(addr, sizeof(T), value);
        return *this;
    }
};

static inline MmioReg<uint8_t> r8(uintptr_t addr)
{
    return MmioReg<uint8_t>{addr};
}

static inline MmioReg<uint16_t> r16(uintptr_t addr)
{
    return MmioReg<uint16_t>{addr};
}

static inline MmioReg<uint32_t> r32(uintptr_t addr)
{
    return MmioReg<uint32_t>{addr};
}

#endif
