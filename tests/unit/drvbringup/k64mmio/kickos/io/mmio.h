// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Stands in for system/include's: every r8 access reaches the test's register model, and only
// an access the code actually performs does. A discarded r8(...) reads nothing, as on silicon.

#ifndef KICKOS_TESTS_UNIT_DRVBRINGUP_K64MMIO_KICKOS_IO_MMIO_H
#define KICKOS_TESTS_UNIT_DRVBRINGUP_K64MMIO_KICKOS_IO_MMIO_H

#include <stdint.h>

namespace kickos::testfix::mmio8
{
    // Defined by the test.
    uint8_t read(uintptr_t addr);
    void write(uintptr_t addr, uint8_t value);

    class Reg8
    {
      public:
        explicit Reg8(uintptr_t addr) : addr_(addr)
        {
        }

        operator uint8_t() const
        {
            return read(addr_);
        }

        Reg8& operator=(uint8_t value)
        {
            write(addr_, value);
            return *this;
        }

      private:
        uintptr_t addr_;
    };
}

static inline kickos::testfix::mmio8::Reg8 r8(uintptr_t addr)
{
    return kickos::testfix::mmio8::Reg8(addr);
}

#endif
