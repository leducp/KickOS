// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Stands in for system/include's: the chip's MMIO accessors over regfile.h's words.

#ifndef KICKOS_TESTS_UNIT_CLOCKBRINGUP_SEAM_KICKOS_IO_MMIO_H
#define KICKOS_TESTS_UNIT_CLOCKBRINGUP_SEAM_KICKOS_IO_MMIO_H

#include <stdint.h>

#include "../../../regfile.h"

static inline volatile uint8_t& r8(uintptr_t addr)
{
    return reinterpret_cast<volatile uint8_t&>(kickos::testfix::regfile::word(addr));
}

static inline volatile uint16_t& r16(uintptr_t addr)
{
    return reinterpret_cast<volatile uint16_t&>(kickos::testfix::regfile::word(addr));
}

static inline volatile uint32_t& r32(uintptr_t addr)
{
    return kickos::testfix::regfile::word(addr);
}

#endif
