// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800/USIC0-CH1 SSC silicon validation through the SPI class <kickos/driver/spi.h>,
// built BOTH WAYS from this one source (see the app's CMakeLists.txt): with KICKOS_SPI_LOCAL
// the client owns the U0C1 window and the USIC0 SR1 line and does its own bring-up,
// otherwise the same calls marshal onto the board's SSC service endpoint.
//
// Data path is the driver's INTERNAL LOOP-BACK (DX0 = own transmitter): there is no external
// SPI device on the bench, so every byte echoes.
//
// Build-only diagnostic: the operator flashes and validates on silicon, so no CTest gate.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <kickos/driver/spi.h>    // the SPI class: kos_spi_bus_open / device_open / transfer
#include <kickos/driver/xmcssc.h> // xmc_spi0_take_endpoint()

#include <stdint.h>
#include <stddef.h>

// Backstops the CMake enforcement-build gate: a granted DEV window is only a real capability
// under PMSA.
#if !KICKOS_HAVE_MPU
#error "xmcssc requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // The single delegated cap lands at child table index 1. Which cap it IS is the build's
    // choice: the service endpoint's SIGNAL copy, or the USIC0 SR1 line.
    constexpr kos_cap_t CLIENT_CAP0 = KOS_SPAWN_DELEGATED_CAP0;

    // The single device on the bench's bus. A client with several devices gives each its own
    // slot (< KOS_BUS_DEV_MAX) and opens each once.
    constexpr uint8_t SPI_SLOT = 0u;

#if KICKOS_SPI_LOCAL
    // USIC0 channel 1, the SSC channel; the console owns channel 0.
    constexpr uintptr_t U0C1_BASE = 0x40030200u;
    constexpr uint32_t U0C1_WINDOW = 0x200u;
    constexpr int USIC0_SR1_IRQ = 85; // RM Table 4-3
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

    // UNPRIVILEGED client. IDENTICAL in both builds below the bus config.
    void spi_client(void* arg)
    {
        struct kos_spi_bus_config bcfg;
#if KICKOS_SPI_LOCAL
        bcfg.base = reinterpret_cast<uintptr_t>(arg); // the granted window, as a VALUE
        bcfg.ep = KOS_CAP_NONE;
        bcfg.irq = CLIENT_CAP0;
#else
        (void)arg;
        bcfg.base = 0u;
        bcfg.ep = CLIENT_CAP0;
        bcfg.irq = KOS_CAP_NONE;
#endif
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

        // 1) Single bytes echo through the on-chip loopback.
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

        // 2) Multi-byte transfer (exercises the per-word IRQ-paced loop and, for CS_HW, the
        //    SOF..EOF frame spanning all words in one MSLS bracket).
        {
            unsigned char const tx[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
            unsigned char buf[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
            struct kos_bus_seg seg = {static_cast<uint16_t>(sizeof(buf)), 0u, 0u};
            int32_t const n = kos_spi_transfer(&dev, &seg, 1u, buf, sizeof(buf));
            report("multi-byte loopback",
                   n == static_cast<int32_t>(sizeof(buf)) and buffers_equal(tx, buf, sizeof(buf)));
        }

        // 3) All-zero tx: the loopback returns 0x00.
        {
            unsigned char buf[4] = {0u, 0u, 0u, 0u};
            struct kos_bus_seg seg = {static_cast<uint16_t>(sizeof(buf)), 0u, 0u};
            int32_t const n = kos_spi_transfer(&dev, &seg, 1u, buf, sizeof(buf));
            report("zero-tx loopback",
                   n == static_cast<int32_t>(sizeof(buf)) and buffer_is(buf, 0x00u, sizeof(buf)));
        }

        // 4) Two segments in ONE CS bracket. The class returns EVERY full-duplex byte, so
        //    the read phase is the tail of the same buffer.
        {
            unsigned char const cmd[3] = {0x03u, 0x00u, 0x64u};
            unsigned char buf[7] = {0x03u, 0x00u, 0x64u, 0u, 0u, 0u, 0u};
            struct kos_bus_seg seg[2] = {{3u, 0u, 0u}, {4u, 0u, 0u}};
            int32_t const n = kos_spi_transfer(&dev, seg, 2u, buf, sizeof(buf));
            report("two-segment transaction (one CS bracket)",
                   n == static_cast<int32_t>(sizeof(buf)) and buffers_equal(cmd, buf, 3)
                       and buffer_is(buf + 3, 0x00u, 4));
        }

        // 5) The API's own ceiling: refused, in every implementation, without clocking.
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
        }
        else
        {
            kos::print("[xmcssc] loopback FAIL (see per-case lines above)\n");
        }

        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
#if KICKOS_SPI_LOCAL
    // Claim the line here: minting needs AUTH_IRQ and the client runs at authority 0. No SSC
    // service may be running in this image, one owner per block.
    kos_cap_t irq = KOS_CAP_NONE;
    if (kos_irq_claim(USIC0_SR1_IRQ, KOS_IRQ_EDGE, &irq) != 0)
    {
        kos::print("[xmcssc] ERROR: irq_claim(USIC0 SR1) failed\n");
    }
    else
    {
        kos_cap_grant const caps[1] = {
            { .source_cap = irq, .rights_mask = KOS_CAP_WAIT },
        };
        auto const c = kos::thread::spawn(
            spi_client, reinterpret_cast<void*>(U0C1_BASE), "xmcssc-cli", 9, KOS_POLICY_FIFO,
            /*quantum_ns=*/0, /*privileged=*/false, /*mem=*/nullptr, /*mem_size=*/0,
            /*stack=*/nullptr, /*stack_size=*/0, reinterpret_cast<void*>(U0C1_BASE), U0C1_WINDOW,
            caps, /*cap_count=*/1);
        if (not c.valid())
        {
            kos::print("[xmcssc] ERROR: client spawn failed\n");
        }
        // Root drops its own copy so the line returns to the pool when the client dies.
        kos_handle_close(irq);
    }
#else
    // The SSC service is already up: take the endpoint the service list recorded.
    kos_cap_t const ep = xmc_spi0_take_endpoint();
    if (ep == KOS_CAP_NONE)
    {
        kos::print("[xmcssc] ERROR: SPI service not up (endpoint unavailable)\n");
    }
    else
    {
        // Delegate a SIGNAL-narrowed copy of E to the spawned client (child index 1).
        kos_cap_grant const caps[1] = {
            { .source_cap = ep, .rights_mask = KOS_CAP_SIGNAL },
        };
        auto const c = kos::thread::spawn_caps(spi_client, nullptr, "xmcssc-cli", 9,
                                               caps, /*cap_count=*/1);
        if (not c.valid())
        {
            kos::print("[xmcssc] ERROR: client spawn failed\n");
        }
        else
        {
            // Drop root's own cap so the driver is the sole recv holder: its death then
            // EPIPE-wakes the client instead of leaving it parked.
            kos_handle_close(ep);
        }
    }
#endif

    kos_cap_t idle = KOS_CAP_NONE;
    (void)kos_sem_create(0, &idle);
    while (true)
    {
        if (idle == KOS_CAP_NONE)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
