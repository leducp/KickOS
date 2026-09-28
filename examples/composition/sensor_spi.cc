// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// M10 GOLDEN EXAMPLE: does not build yet. `kos_self_t`, the `kos_grant_*` lookups and the
// `kos_window_*` accessors are what M10 adds; everything else is today's API.
//
// The sensor task. It is a CLIENT of the SPI bus service and the SERVER of /svc/sensor. Its
// "measurement" is a counter clocked through the bus's internal loopback, which is what lets
// this example run on a bare Relax Kit. It holds no authority, no device window and no line:
// the bus service owns the channel, and the composition is the only place that says so.

#include <kickos/sys.h>
#include <kickos/driver/spi.h>

#include "sample.h"

#include <stdint.h>

namespace
{
    // Mode 0, hardware chip select: the only profile the XMC SSC channel accepts, its rate
    // and clock phase being fixed when the service brings the channel up.
    constexpr uint8_t SPI_SLOT = 0u;

    // The loopback returns what was sent, so a counter stands in for a changing reading.
    uint32_t measure(struct kos_spi_device* dev, uint32_t counter, bool* ok)
    {
        unsigned char buf[4] = {
            static_cast<unsigned char>(counter >> 0),
            static_cast<unsigned char>(counter >> 8),
            static_cast<unsigned char>(counter >> 16),
            static_cast<unsigned char>(counter >> 24),
        };
        struct kos_bus_seg seg = {sizeof(buf), 0u, 0u};
        *ok = kos_spi_transfer(dev, &seg, 1u, buf, sizeof(buf)) == static_cast<int32_t>(sizeof(buf));
        return (static_cast<uint32_t>(buf[0]) << 0) |
               (static_cast<uint32_t>(buf[1]) << 8) |
               (static_cast<uint32_t>(buf[2]) << 16) |
               (static_cast<uint32_t>(buf[3]) << 24);
    }
}

extern "C" void sensor_main(kos_self_t const* self)
{
    // Both capabilities were delegated by the init before this task ran; the names are the
    // paths the composition declares. A name this task was not given answers KOS_CAP_NONE.
    struct kos_spi_bus_config bcfg = {};
    bcfg.ep = kos_grant_endpoint(self, "/svc/spi0");
    bcfg.irq = KOS_CAP_NONE;
    bcfg.notify = KOS_CAP_NONE;
    kos_cap_t const served = kos_grant_endpoint(self, "/svc/sensor");
    // Declared shared memory, mapped by the init before this task ran, never by hand.
    kos_window_t const history_window = kos_grant_mem(self, "/shm/history");
    auto const hist = static_cast<history*>(kos_window_addr(history_window));
    if (hist == nullptr or kos_window_size(history_window) < sizeof(history))
    {
        return;
    }

    struct kos_spi_bus bus;
    struct kos_spi_device dev;
    struct kos_spi_device_config dcfg = {};
    dcfg.slot = SPI_SLOT;
    dcfg.mode = 0u;
    dcfg.cs_policy = KOS_BUS_CS_HW;
    // Two levels: the bus, here the service's endpoint, then one device on it, with its own
    // chip select and profile. A failed open is a dead sensor: returning ends the task, and
    // the restart policy in the composition decides what happens next.
    if (kos_spi_bus_open(&bus, &bcfg) != 0)
    {
        return;
    }
    // Answers the clock rate the device will actually run at, so success is a positive rate.
    if (kos_spi_device_open(&dev, &bus, &dcfg) <= 0)
    {
        return;
    }

    uint32_t counter = 0;
    struct kos_reply_recv_opts opts;
    kos_cap_t reply = KOS_CAP_NONE;
    struct sample s = {};
    while (true)
    {
        kos_reply_recv_opts_init(&opts, served, 0u, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(reply, &s, kos_call_lens_pack(sizeof(s), 0u), &opts);
        reply = KOS_CAP_NONE;
        if (n < 0)
        {
            continue;
        }
        reply = opts.info.reply_cap;

        bool ok = false;
        s.value = measure(&dev, counter++, &ok);
        if (ok)
        {
            history_append(hist, s.value);
        }
        else
        {
            // Say nothing about why: the consumer judges the reading, not this task.
            kos_handle_close(reply);
            reply = KOS_CAP_NONE;
        }
    }
}
