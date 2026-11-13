// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The register file a chip's clock bring-up runs against on the host. Every MMIO access lands in a
// word keyed by its address, and every bounded wait succeeds, its bits written to the value it
// waited for, unless the case planted it to time out. A status register the code reads without a
// wait holds what the case seeded, or what the case's access hook writes there before each access.

#ifndef KICKOS_TESTS_UNIT_CLOCKBRINGUP_REGFILE_H
#define KICKOS_TESTS_UNIT_CLOCKBRINGUP_REGFILE_H

#include <stdint.h>

namespace kickos::testfix::regfile
{
    // Every word back to 0, every plant gone.
    void reset();

    void seed(uintptr_t address, uint32_t value);
    uint32_t value(uintptr_t address);
    bool touched(uintptr_t address);

    // A wait on `address` over exactly `mask` times out, for any value it waits for or only for
    // `want`.
    void plant_timeout(uintptr_t address, uint32_t mask);
    void plant_timeout(uintptr_t address, uint32_t mask, uint32_t want);

    // Runs before every access through word(), with the address it reaches.
    void on_access(void (*hook)(uintptr_t address));

    // How many waits ran, and how many timed out on a plant.
    uint32_t waits();
    uint32_t timeouts();

    volatile uint32_t& word(uintptr_t address);
    bool wait(void const volatile* reg, uint32_t mask, uint32_t want, uint32_t width);
}

#endif
