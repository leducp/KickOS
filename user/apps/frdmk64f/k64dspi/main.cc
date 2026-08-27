// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/DSPI0 silicon validation through the SPI class <kickos/driver/spi.h>. A CLIENT of the
// DSPI0 service the board service list brings up before main: SPI_BACKEND is
// kickos_spi_proxy, so this app touches no MMIO, no CS and no grant.
//
// Two build modes over the SAME service:
//   DEFAULT: LAN9252 BYTE_TEST probe, EasyCAT shield on the Arduino header.
//   K64DSPI_LOOPBACK=ON: SOUT(PTD2)->SIN(PTD3) loopback (jumper, no shield).
//
// Build-only diagnostic: the operator flashes and validates on silicon.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <kickos/driver/k64dspi.h> // k64dspi_take_endpoint()
#include <kickos/driver/spi.h>     // the SPI class: kos_spi_bus_open / device_open / transfer

#include <stdint.h>
#include <stddef.h>

// Backstops the CMake enforcement-build gate: the MMIO-grant seam needs enforcement.
#if !KICKOS_HAVE_MPU
#error "k64dspi requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // The delegated endpoint SIGNAL cap lands at the client's child table index 1.
    constexpr kos_cap_t SPI_EP = KOS_SPAWN_DELEGATED_CAP0;

    // The single device on the bench's bus; a slot is per device and opened once.
    constexpr uint8_t SPI_SLOT = 0;

    // Open the bus over the service endpoint and issue the one device handle this app uses.
    // Returns the achieved bit clock, or a negative kos_errno; *dev is valid on success only.
    int32_t open_device(struct kos_spi_bus* bus, struct kos_spi_device* dev, uint32_t hz,
                        uint8_t cs_policy)
    {
        struct kos_spi_bus_config bcfg;
        bcfg.base = 0u; // a proxy reaches no register window
        bcfg.ep = SPI_EP;
        bcfg.irq = KOS_CAP_NONE;
        int32_t const brc = kos_spi_bus_open(bus, &bcfg);
        if (brc < 0)
        {
            return brc;
        }

        struct kos_spi_device_config dcfg;
        dcfg.hz = hz;
        dcfg.slot = SPI_SLOT;
        dcfg.mode = 0u; // SPI mode 0, MSB first
        dcfg.word_bits = 8u;
        dcfg.cs_policy = cs_policy;
        // ONE CS pin, PTC4. cs_index names a pin slot, and this engine drives exactly slot 0,
        // so naming the pin number here would be refused rather than ignored.
        dcfg.cs_index = 0u;
        dcfg.rsv[0] = 0u;
        dcfg.rsv[1] = 0u;
        dcfg.rsv[2] = 0u;
        return kos_spi_device_open(dev, bus, &dcfg);
    }

#if defined(K64DSPI_LOOPBACK)

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
        ksnprintf(s, sizeof(s), "[k64dspi] %s: %s\n", label, verdict);
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

    // UNPRIVILEGED client. No CS: the loopback jumper has none.
    void spi_client(void*)
    {
        struct kos_spi_bus bus;
        struct kos_spi_device dev;
        int32_t const hz = open_device(&bus, &dev, /*hz=*/1000000u, KOS_BUS_CS_NONE);
        {
            char s[80];
            ksnprintf(s, sizeof(s), "[k64dspi] device open rc=%d achieved=%lu Hz\n",
                      static_cast<int>(hz), static_cast<unsigned long>(dev.hz));
            kos::print(s);
        }
        report("device open", hz > 0);

        // 1) Single bytes echo through the loopback.
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

        // 2) Multi-byte transfer larger than the TX FIFO (exercises the refill loop).
        {
            unsigned char const tx[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
            unsigned char buf[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
            struct kos_bus_seg seg = {static_cast<uint16_t>(sizeof(buf)), 0u, 0u};
            int32_t const n = kos_spi_transfer(&dev, &seg, 1u, buf, sizeof(buf));
            report("multi-byte (>FIFO) loopback",
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

        // 4) Two segments in ONE CS bracket. The class returns EVERY full-duplex byte, so the
        //    read phase is the tail of the same buffer.
        {
            unsigned char const cmd[3] = {0x03u, 0x00u, 0x64u};
            unsigned char buf[7] = {0x03u, 0x00u, 0x64u, 0u, 0u, 0u, 0u};
            struct kos_bus_seg seg[2] = {{3u, 0u, 0u}, {4u, 0u, 0u}};
            int32_t const n = kos_spi_transfer(&dev, seg, 2u, buf, sizeof(buf));
            report("two-segment transaction (one CS bracket)",
                   n == static_cast<int32_t>(sizeof(buf)) and buffers_equal(cmd, buf, 3)
                       and buffer_is(buf + 3, 0x00u, 4));
        }

        if (g_fails == 0)
        {
            kos::print("[k64dspi] loopback PASS (call/reply SPI service echoes tx==rx)\n");
        }
        else
        {
            kos::print("[k64dspi] loopback FAIL (see per-case lines above)\n");
        }

        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }

#else // LAN9252 BYTE_TEST probe (default)

    constexpr uint32_t LAN9252_BYTE_TEST = 0x87654321u;
    constexpr uint16_t BYTE_TEST_ADDR = 0x0064u;
    constexpr unsigned char LAN9252_READ = 0x03u;
    constexpr int PROBE_RETRIES = 8;
    constexpr uint64_t RETRY_DELAY_NS = 10000000ull; // 10 ms ESC settle

    // One BYTE_TEST read: cmd (0x03 + 16-bit addr big-endian) then 4 read bytes, under ONE
    // CS bracket. The read phase is the tail of the same buffer, and its bytes arrive
    // LSB-first.
    uint32_t read_byte_test(struct kos_spi_device* dev, bool* ok)
    {
        unsigned char buf[7];
        buf[0] = LAN9252_READ;
        buf[1] = static_cast<unsigned char>((BYTE_TEST_ADDR >> 8) & 0xFFu);
        buf[2] = static_cast<unsigned char>(BYTE_TEST_ADDR & 0xFFu);
        buf[3] = 0u;
        buf[4] = 0u;
        buf[5] = 0u;
        buf[6] = 0u;

        struct kos_bus_seg seg[2] = {{3u, 0u, 0u}, {4u, 0u, 0u}};
        int32_t const n = kos_spi_transfer(dev, seg, 2u, buf, sizeof(buf));
        *ok = (n == static_cast<int32_t>(sizeof(buf)));

        uint32_t val = static_cast<uint32_t>(buf[3]);
        val |= static_cast<uint32_t>(buf[4]) << 8;
        val |= static_cast<uint32_t>(buf[5]) << 16;
        val |= static_cast<uint32_t>(buf[6]) << 24;
        return val;
    }

    // UNPRIVILEGED client: open the device (10 MHz, GPIO CS) then the BYTE_TEST probe.
    void spi_client(void*)
    {
        struct kos_spi_bus bus;
        struct kos_spi_device dev;
        int32_t const hz = open_device(&bus, &dev, /*hz=*/10000000u, KOS_BUS_CS_GPIO);
        {
            char s[80];
            ksnprintf(s, sizeof(s), "[k64dspi] device open rc=%d achieved=%lu Hz\n",
                      static_cast<int>(hz), static_cast<unsigned long>(dev.hz));
            kos::print(s);
        }

        bool pass = false;
        for (int attempt = 1; attempt <= PROBE_RETRIES and not pass; attempt++)
        {
            bool ok = false;
            uint32_t val = read_byte_test(&dev, &ok);

            char const* xfer = "OK";
            if (not ok)
            {
                xfer = "ERR";
            }
            char s[96];
            ksnprintf(s, sizeof(s), "[k64dspi] BYTE_TEST attempt %d: 0x%lx (xfer %s)\n",
                      attempt, static_cast<unsigned long>(val), xfer);
            kos::print(s);

            if (ok and val == LAN9252_BYTE_TEST)
            {
                pass = true;
            }
            else
            {
                kos_sleep_ns(RETRY_DELAY_NS);
            }
        }

        if (pass)
        {
            kos::print("[k64dspi] LAN9252 BYTE_TEST PASS: ESC SPI link OK "
                       "(read 0x87654321 through the call/reply SPI service)\n");
        }
        else
        {
            kos::print("[k64dspi] LAN9252 BYTE_TEST FAIL: no valid signature; check CS "
                       "(D9/PTC4), baud/mode, or shield seating\n");
        }

        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }

#endif
}

int main(int, char**)
{
    // DSPI0 is already up: take the endpoint the service list recorded.
    kos_cap_t const ep = k64dspi_take_endpoint();
    if (ep == KOS_CAP_NONE)
    {
        kos::print("[k64dspi] ERROR: SPI service not up (endpoint unavailable)\n");
    }
    else
    {
        // Delegate a SIGNAL-narrowed copy of E to the spawned client (child index 1).
        kos_cap_grant const caps[1] = {
            { .source_cap = ep, .rights_mask = KOS_CAP_SIGNAL },
        };
        auto const c = kos::thread::create_caps(spi_client, nullptr, "k64spi-cli", 9,
                                                caps, /*cap_count=*/1);
        if (not c.valid())
        {
            kos::print("[k64dspi] ERROR: client spawn failed\n");
        }
        else
        {
            // Drop root's own cap so the driver is the sole recv holder: its death then
            // EPIPE-wakes the client instead of leaving it parked.
            kos_handle_close(ep);
        }
    }

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
