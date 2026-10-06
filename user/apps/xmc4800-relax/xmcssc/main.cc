// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800/USIC0-CH1 SSC silicon validation through the SPI class <kickos/driver/spi.h>, run by the
// task's entry over the grants its composition hands it. Built BOTH WAYS from this one source: with
// KICKOS_SPI_LOCAL (KICKOS_SPI_LOCAL_ENGINE=ON) the client owns the channel window and its line and
// links the local engine, otherwise the same calls marshal onto the packaged xmcssc service
// endpoint. The data path is the engine's internal loop-back (DX0 = own transmitter), so every byte
// echoes with no external SPI device on the bench.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <kickos/driver/spi.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if !KICKOS_HAVE_MPU
#error "xmcssc requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // The single device on the bench's bus; a slot is per device and stays < KOS_BUS_DEV_MAX.
    constexpr uint8_t SPI_SLOT = 0u;

#if KICKOS_SPI_LOCAL
    constexpr uint32_t U0C1_WINDOW = 0x200u;
#endif

    int g_fails = 0;

    void report(char const* label, bool ok)
    {
        char s[80];
        char const* verdict = "PASS";
        if (not ok)
        {
            verdict = "FAIL";
            g_fails++;
        }
        ksnprintf(s, sizeof(s), "[xmcssc] %s: %s\n", label, verdict);
        kos::print(s);
    }

    bool buffers_equal(unsigned char const* a, unsigned char const* b, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            if (a[i] != b[i])
            {
                return false;
            }
        }
        return true;
    }

    bool buffer_is(unsigned char const* a, unsigned char v, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            if (a[i] != v)
            {
                return false;
            }
        }
        return true;
    }

    // The client body, IDENTICAL in both builds below the bus config it is handed.
    int run_client(struct kos_spi_bus_config const* bcfg_in)
    {
        struct kos_spi_bus_config bcfg = *bcfg_in;
        struct kos_spi_bus bus;
        int32_t const brc = kos_spi_bus_open(&bus, &bcfg);
        report("bus open", brc == 0);
        if (brc != 0)
        {
            char s[80];
            ksnprintf(s, sizeof(s), "[xmcssc] bus open rc=%d\n", static_cast<int>(brc));
            kos::print(s);
        }

        // hz = 0 is the only rate this channel accepts: its baud profile is fixed at bring-up
        // and BRG also pins the clock polarity and phase, so kos_spi_device_open reports the
        // rate it read back rather than echoing a request it could not honour.
        struct kos_spi_device_config dcfg;
        dcfg.hz = 0u;
        dcfg.slot = SPI_SLOT;
        dcfg.mode = 0u; // SPI mode 0, MSB first
        dcfg.word_bits = 8u;
        dcfg.cs_policy = KOS_BUS_CS_HW; // hardware MSLS/SELO0, held across the frame
        dcfg.cs_index = 0u;             // SELO0, the channel's only CS line
        dcfg.rsv[0] = 0u;
        dcfg.rsv[1] = 0u;
        dcfg.rsv[2] = 0u;

        struct kos_spi_device dev;
        int32_t const hz = kos_spi_device_open(&dev, &bus, &dcfg);
        {
            char s[80];
            ksnprintf(s, sizeof(s), "[xmcssc] device open rc=%d achieved=%lu Hz\n",
                      static_cast<int>(hz), static_cast<unsigned long>(dev.hz));
            kos::print(s);
        }
        report("device open", hz > 0);

        {
            unsigned char const pattern[] = {0xA5u, 0x3Cu, 0x00u, 0xFFu};
            struct kos_bus_seg seg = {1u, 0u, 0u};
            bool ok = true;
            for (unsigned i = 0; i < sizeof(pattern); i++)
            {
                unsigned char buf[1] = {pattern[i]};
                int32_t const n = kos_spi_transfer(&dev, &seg, 1u, buf, 1u);
                if (n != 1 or buf[0] != pattern[i])
                {
                    ok = false;
                }
            }
            report("single-byte loopback", ok);
        }

        // Exercises the per-word IRQ-paced loop and, for CS_HW, the SOF..EOF frame spanning
        // all words in one MSLS bracket.
        {
            unsigned char const tx[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
            unsigned char buf[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
            struct kos_bus_seg seg = {static_cast<uint16_t>(sizeof(buf)), 0u, 0u};
            int32_t const n = kos_spi_transfer(&dev, &seg, 1u, buf, sizeof(buf));
            report("multi-byte loopback",
                   n == static_cast<int32_t>(sizeof(buf)) and buffers_equal(tx, buf, sizeof(buf)));
        }

        {
            unsigned char buf[4] = {0u, 0u, 0u, 0u};
            struct kos_bus_seg seg = {static_cast<uint16_t>(sizeof(buf)), 0u, 0u};
            int32_t const n = kos_spi_transfer(&dev, &seg, 1u, buf, sizeof(buf));
            report("zero-tx loopback",
                   n == static_cast<int32_t>(sizeof(buf)) and buffer_is(buf, 0x00u, sizeof(buf)));
        }

        // Two segments in ONE CS bracket. The class returns EVERY full-duplex byte, so the
        // read phase is the tail of the same buffer.
        {
            unsigned char const cmd[3] = {0x03u, 0x00u, 0x64u};
            unsigned char buf[7] = {0x03u, 0x00u, 0x64u, 0u, 0u, 0u, 0u};
            struct kos_bus_seg seg[2] = {{3u, 0u, 0u}, {4u, 0u, 0u}};
            int32_t const n = kos_spi_transfer(&dev, seg, 2u, buf, sizeof(buf));
            report("two-segment transaction (one CS bracket)",
                   n == static_cast<int32_t>(sizeof(buf)) and buffers_equal(cmd, buf, 3)
                       and buffer_is(buf + 3, 0x00u, 4));
        }

        // The API's own ceiling: refused, in every implementation, without clocking.
        {
            unsigned char buf[8] = {0};
            struct kos_bus_seg seg = {static_cast<uint16_t>(KOS_SPI_XFER_MAX + 1), 0u, 0u};
            int32_t const n =
                kos_spi_transfer(&dev, &seg, 1u, buf, static_cast<uint32_t>(KOS_SPI_XFER_MAX) + 1u);
            report("oversized transfer refused", n == -KOS_EINVAL);
        }

        if (g_fails == 0)
        {
            kos::print("[xmcssc] loopback PASS (the SSC bus echoes tx == rx)\n");
            return 0;
        }
        kos::print("[xmcssc] loopback FAIL (see per-case lines above)\n");
        return 1;
    }
}

extern "C" void xmcssc_main(kos_self_t const* self)
{
    struct kos_spi_bus_config bcfg;
    bcfg.notify_bit = 0;
#if KICKOS_SPI_LOCAL
    // The client owns the channel: the window and the USIC0 line its composition grants, and a
    // notification it binds the line to.
    kos_window_t const window = kos_grant_mmio(self, "/dev/usic0/ch1");
    kos_line_t const line = kos_grant_irq(self, "irq");
    uintptr_t const win = reinterpret_cast<uintptr_t>(kos_window_addr(window));
    if (win == 0u or kos_window_size(window) < U0C1_WINDOW or line.cap == KOS_CAP_NONE)
    {
        kos::print("[xmcssc] ERROR: no /dev/usic0/ch1 window or irq line\n");
        exit(1);
    }
    kos_cap_t note = KOS_CAP_NONE;
    int rc = kos_notify_create(&note);
    if (rc == 0)
    {
        rc = kos_notify_bind(note);
    }
    if (rc == 0)
    {
        rc = kos_irq_bind_notify(line.cap, note);
    }
    if (rc != 0)
    {
        char e[64];
        ksnprintf(e, sizeof(e), "[xmcssc] ERROR: the line's notification rc %d\n", rc);
        kos::print(e);
        exit(1);
    }
    bcfg.base = win;
    bcfg.ep = KOS_CAP_NONE;
    bcfg.irq = line.cap;
    bcfg.notify = note;
    bcfg.irq_index = line.index;
#else
    // The client reaches the channel over the endpoint the packaged xmcssc serves.
    kos_cap_t const ep = kos_grant_endpoint(self, "/svc/spi0");
    if (ep == KOS_CAP_NONE)
    {
        kos::print("[xmcssc] ERROR: no /svc/spi0 endpoint\n");
        exit(1);
    }
    bcfg.base = 0u;
    bcfg.ep = ep;
    bcfg.irq = KOS_CAP_NONE;
    bcfg.notify = KOS_CAP_NONE;
    bcfg.irq_index = 0u;
#endif
    exit(run_client(&bcfg));
}
