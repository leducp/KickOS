// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Stands in for arch/include's: a chip's register poll answered by regfile.h, which times out
// only the waits a case planted. Same signatures as the real header.

#ifndef KICKOS_TESTS_UNIT_CLOCKBRINGUP_SEAM_KICKOS_ARCH_BOUNDED_WAIT_H
#define KICKOS_TESTS_UNIT_CLOCKBRINGUP_SEAM_KICKOS_ARCH_BOUNDED_WAIT_H

#include <stdint.h>

#include "../../../regfile.h"

namespace kickos::arch
{
    template <typename T>
    inline bool wait_bits(T volatile& reg, T mask, T want)
    {
        return kickos::testfix::regfile::wait(&reg, mask, want, sizeof(T));
    }

    inline bool wait_set(uint32_t volatile& reg, uint32_t mask)
    {
        return wait_bits<uint32_t>(reg, mask, mask);
    }

    inline bool wait_set(uint8_t volatile& reg, uint8_t mask)
    {
        return wait_bits<uint8_t>(reg, mask, mask);
    }

    inline bool wait_clear(uint32_t volatile& reg, uint32_t mask)
    {
        return wait_bits<uint32_t>(reg, mask, 0u);
    }
}

#endif
