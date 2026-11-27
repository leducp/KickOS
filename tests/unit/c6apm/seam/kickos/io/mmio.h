// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Stands in for system/include's: r32 over led_rmt.cc's fake register file.

#ifndef KICKOS_TESTS_UNIT_C6APM_SEAM_KICKOS_IO_MMIO_H
#define KICKOS_TESTS_UNIT_C6APM_SEAM_KICKOS_IO_MMIO_H

#include <stdint.h>

namespace kickos::testfix::ledrmt
{
    volatile uint32_t& word(uintptr_t address);
}

static inline volatile uint32_t& r32(uintptr_t addr)
{
    return kickos::testfix::ledrmt::word(addr);
}

#endif
