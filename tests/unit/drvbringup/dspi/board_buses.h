// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// frdmk64f's generated bus table, in the shape the chip generator emits it: DSPI0 with one GPIO
// chip select on PTC4. The bus base is the host buffer dspi_mmap.h names, so the table is not
// constexpr here.

#ifndef KICKOS_TESTS_UNIT_DRVBRINGUP_DSPI_BOARD_BUSES_H
#define KICKOS_TESTS_UNIT_DRVBRINGUP_DSPI_BOARD_BUSES_H

#include <stdint.h>

namespace kickos::mk64f::board
{
    struct gpio_pin
    {
        uint32_t port;
        uint32_t bit;
    };

    struct bus_pin
    {
        uint32_t port;
        uint32_t bit;
        uint32_t select;
    };

    struct bus
    {
        char const* name;
        uintptr_t base;
        bus_pin const* pins;
        uint32_t pin_count;
        gpio_pin const* chip_selects;
        uint32_t chip_select_count;
    };

    inline constexpr bus_pin spi0_pins[] = {
        {3u, 1u, 2u},
        {3u, 2u, 2u},
        {3u, 3u, 2u},
    };
    inline constexpr gpio_pin spi0_chip_selects[] = {
        {2u, 4u},
    };

    inline bus const bus_rows[] = {
        {"spi0", mmap::DSPI0_BASE, spi0_pins, 3u, spi0_chip_selects, 1u},
    };
    inline bus const* const buses = bus_rows;
    inline constexpr uint32_t bus_count = 1u;
}

#endif
