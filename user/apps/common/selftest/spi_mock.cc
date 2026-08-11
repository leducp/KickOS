// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A MOCK backend of the SPI class <kickos/driver/spi.h>, letting the whole bus-service request
// surface be exercised with NO device, on any board.
//
// A transfer fills the buffer with the word size of the HANDLE it was given, so
// t_bus_device_slots can prove a transfer on slot 0 still reads back slot 0's word size after
// slot 1 was opened. With one global profile the second open would win.
//
// This TU holds the PUBLIC class names alone. A selftest image on a board whose service list
// brings up an SPI driver also carries that chip engine, which is legal only because a service
// driver renames its four class symbols; tests/check_class_backend.sh asserts it per link.

#include <kickos/driver/spi.h>

#include <kickos/sys/errno.h>

#include <stdint.h>

namespace
{
    // kos_spi_bus.base is a register window to a real engine and merely a liveness marker
    // here; nothing dereferences it.
    constexpr uintptr_t MOCK_BUS_LIVE = 1u;
}

extern "C"
{

int32_t kos_spi_bus_open(struct kos_spi_bus* b, struct kos_spi_bus_config const* cfg)
{
    b->base = MOCK_BUS_LIVE;
    b->ep = cfg->ep;
    b->irq = cfg->irq;
    return 0;
}

int32_t kos_spi_device_open(struct kos_spi_device* d, struct kos_spi_bus* b,
                            struct kos_spi_device_config const* cfg)
{
    if (b->base != MOCK_BUS_LIVE)
    {
        return -KOS_EINVAL;
    }
    if (cfg->slot >= KOS_BUS_DEV_MAX)
    {
        return -KOS_EINVAL;
    }
    // Unlike the silicon engines this accepts any frame size: the value is the thing under
    // test rather than something clocked.
    if (cfg->word_bits < 4u or cfg->word_bits > 16u)
    {
        return -KOS_ENOTSUP;
    }
    if (cfg->hz == 0u)
    {
        return -KOS_ENOTSUP; // no divider to read a rate back from
    }
    d->bus = b;
    d->hz = cfg->hz;
    d->prog[0] = 0u;
    d->prog[1] = 0u;
    d->slot = cfg->slot;
    d->mode = cfg->mode;
    d->word_bits = cfg->word_bits;
    d->cs_policy = cfg->cs_policy;
    d->cs_index = cfg->cs_index;
    d->rsv[0] = 0u;
    d->rsv[1] = 0u;
    d->rsv[2] = 0u;
    return static_cast<int32_t>(cfg->hz);
}

int32_t kos_spi_transfer(struct kos_spi_device* d, struct kos_bus_seg const* seg, uint8_t nseg,
                         unsigned char* buf, uint32_t len)
{
    int32_t const bad = kos_spi_seg_check(seg, nseg, len);
    if (bad != 0)
    {
        return bad;
    }
    if (d->bus == nullptr or d->bus->base != MOCK_BUS_LIVE)
    {
        return -KOS_EINVAL;
    }
    for (uint32_t i = 0; i < len; i++)
    {
        buf[i] = d->word_bits;
    }
    return static_cast<int32_t>(len);
}

int32_t kos_spi_bus_close(struct kos_spi_bus* b)
{
    b->base = 0u;
    return 0;
}

}
