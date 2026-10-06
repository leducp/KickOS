// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/DSPI0 silicon validation through the SPI class <kickos/driver/spi.h>, run by the task's
// entry over the grants its composition hands it. Built BOTH WAYS from this one source: with
// KICKOS_SPI_LOCAL (KICKOS_SPI_LOCAL_ENGINE=ON) the client owns the DSPI0 window, muxes the bus
// pins the board file wires and links the local engine, otherwise the same calls marshal onto
// the packaged k64dspi service endpoint.
//
// Two build modes over the same bus:
//   DEFAULT: LAN9252 BYTE_TEST probe, EasyCAT shield on the Arduino header.
//   K64DSPI_LOOPBACK=ON: SOUT(PTD2)->SIN(PTD3) loopback (jumper, no shield).
// FAIL is printed only for an error no absent fitting explains: a refused open or a transfer that
// did not move every byte. Completed transfers that read back the wrong bytes print MISMATCH.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <kickos/driver/spi.h>
#if KICKOS_SPI_LOCAL
#include <kickos/chip_mmap.h>
#include <kickos/driver/k64dspi.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if !KICKOS_HAVE_MPU
#error "k64dspi requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // The single device on the bench's bus; a slot is per device and opened once.
    constexpr uint8_t SPI_SLOT = 0;

    // Open the bus and issue the one device handle this app uses. Returns the achieved bit
    // clock, or a negative kos_errno; *dev is valid on success only.
    int32_t open_device(struct kos_spi_bus_config const* bcfg, struct kos_spi_bus* bus,
                        struct kos_spi_device* dev, uint32_t hz, uint8_t cs_policy)
    {
        int32_t const brc = kos_spi_bus_open(bus, bcfg);
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
        // A position among the board bus's chip selects, not a pin number: 0 is D9/PTC4.
        dcfg.cs_index = 0u;
        dcfg.rsv[0] = 0u;
        dcfg.rsv[1] = 0u;
        dcfg.rsv[2] = 0u;
        return kos_spi_device_open(dev, bus, &dcfg);
    }

    // 0 for a transfer that moved all <len> bytes, else nonzero: the count, or -1 for none.
    int32_t xfer_rc(int32_t n, size_t len)
    {
        if (n == static_cast<int32_t>(len))
        {
            return 0;
        }
        if (n == 0)
        {
            return -1;
        }
        return n;
    }

    // Returns 0, or the open's negative kos_errno.
    int32_t print_open(int32_t hz, struct kos_spi_device const* dev)
    {
        int32_t rc = 0;
        unsigned long achieved = 0ul;
        if (hz > 0)
        {
            achieved = static_cast<unsigned long>(dev->hz);
        }
        else
        {
            rc = hz;
            if (rc == 0)
            {
                rc = -1;
            }
        }
        char s[80];
        ksnprintf(s, sizeof(s), "[k64dspi] device open rc=%d achieved=%lu Hz\n", static_cast<int>(rc),
                  achieved);
        kos::print(s);
        return rc;
    }

#if defined(K64DSPI_LOOPBACK)

    int g_fails = 0;
    int g_mismatches = 0;

    void report(char const* label, int32_t rc, bool echoed)
    {
        char s[96];
        if (rc != 0)
        {
            g_fails++;
            ksnprintf(s, sizeof(s), "[k64dspi] %s: FAIL (rc=%d)\n", label, static_cast<int>(rc));
        }
        else if (not echoed)
        {
            g_mismatches++;
            ksnprintf(s, sizeof(s), "[k64dspi] %s: MISMATCH\n", label);
        }
        else
        {
            ksnprintf(s, sizeof(s), "[k64dspi] %s: PASS\n", label);
        }
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

    // No CS: the loopback jumper has none.
    int run_client(struct kos_spi_bus_config const* bcfg)
    {
        struct kos_spi_bus bus;
        struct kos_spi_device dev;
        int32_t const hz = open_device(bcfg, &bus, &dev, /*hz=*/1000000u, KOS_BUS_CS_NONE);
        report("device open", print_open(hz, &dev), true);

        // 1) Single bytes echo through the loopback.
        {
            unsigned char const pattern[] = {0xA5u, 0x3Cu, 0x00u, 0xFFu};
            struct kos_bus_seg seg = {1u, 0u, 0u};
            int32_t rc = 0;
            bool echoed = true;
            for (unsigned i = 0; i < sizeof(pattern) and rc == 0; i++)
            {
                unsigned char buf[1] = {pattern[i]};
                rc = xfer_rc(kos_spi_transfer(&dev, &seg, 1u, buf, 1u), 1u);
                if (buf[0] != pattern[i])
                {
                    echoed = false;
                }
            }
            report("single-byte loopback", rc, echoed);
        }

        // 2) Multi-byte transfer larger than the TX FIFO (exercises the refill loop).
        {
            unsigned char const tx[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
            unsigned char buf[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
            struct kos_bus_seg seg = {static_cast<uint16_t>(sizeof(buf)), 0u, 0u};
            int32_t const n = kos_spi_transfer(&dev, &seg, 1u, buf, sizeof(buf));
            report("multi-byte (>FIFO) loopback", xfer_rc(n, sizeof(buf)),
                   buffers_equal(tx, buf, sizeof(buf)));
        }

        // 3) All-zero tx: the loopback returns 0x00.
        {
            unsigned char buf[4] = {0u, 0u, 0u, 0u};
            struct kos_bus_seg seg = {static_cast<uint16_t>(sizeof(buf)), 0u, 0u};
            int32_t const n = kos_spi_transfer(&dev, &seg, 1u, buf, sizeof(buf));
            report("zero-tx loopback", xfer_rc(n, sizeof(buf)), buffer_is(buf, 0x00u, sizeof(buf)));
        }

        // 4) Two segments in ONE CS bracket. The class returns EVERY full-duplex byte, so the
        //    read phase is the tail of the same buffer.
        {
            unsigned char const cmd[3] = {0x03u, 0x00u, 0x64u};
            unsigned char buf[7] = {0x03u, 0x00u, 0x64u, 0u, 0u, 0u, 0u};
            struct kos_bus_seg seg[2] = {{3u, 0u, 0u}, {4u, 0u, 0u}};
            int32_t const n = kos_spi_transfer(&dev, seg, 2u, buf, sizeof(buf));
            report("two-segment transaction (one CS bracket)", xfer_rc(n, sizeof(buf)),
                   buffers_equal(cmd, buf, 3) and buffer_is(buf + 3, 0x00u, 4));
        }

        if (g_fails != 0)
        {
            kos::print("[k64dspi] loopback FAIL (see per-case lines above)\n");
            return 1;
        }
        if (g_mismatches != 0)
        {
            kos::print("[k64dspi] loopback MISMATCH (every transfer completed, rx != tx)\n");
            return 1;
        }
        kos::print("[k64dspi] loopback PASS (the SPI bus echoes tx == rx)\n");
        return 0;
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
    uint32_t read_byte_test(struct kos_spi_device* dev, int32_t* rc)
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
        *rc = xfer_rc(kos_spi_transfer(dev, seg, 2u, buf, sizeof(buf)), sizeof(buf));

        uint32_t val = static_cast<uint32_t>(buf[3]);
        val |= static_cast<uint32_t>(buf[4]) << 8;
        val |= static_cast<uint32_t>(buf[5]) << 16;
        val |= static_cast<uint32_t>(buf[6]) << 24;
        return val;
    }

    // Open the device (10 MHz, GPIO CS), then the BYTE_TEST probe.
    int run_client(struct kos_spi_bus_config const* bcfg)
    {
        struct kos_spi_bus bus;
        struct kos_spi_device dev;
        int32_t const hz = open_device(bcfg, &bus, &dev, /*hz=*/10000000u, KOS_BUS_CS_GPIO);
        print_open(hz, &dev);

        bool pass = false;
        int32_t rc = 0;
        for (int attempt = 1; attempt <= PROBE_RETRIES and not pass and rc == 0; attempt++)
        {
            uint32_t val = read_byte_test(&dev, &rc);

            char s[96];
            if (rc == 0)
            {
                ksnprintf(s, sizeof(s), "[k64dspi] BYTE_TEST attempt %d: 0x%lx (xfer OK)\n", attempt,
                          static_cast<unsigned long>(val));
            }
            else
            {
                ksnprintf(s, sizeof(s), "[k64dspi] BYTE_TEST attempt %d: (xfer ERR rc=%d)\n", attempt,
                          static_cast<int>(rc));
            }
            kos::print(s);

            if (rc == 0 and val == LAN9252_BYTE_TEST)
            {
                pass = true;
            }
            else if (rc == 0)
            {
                kos_sleep_ns(RETRY_DELAY_NS);
            }
        }

        if (pass)
        {
            kos::print("[k64dspi] LAN9252 BYTE_TEST PASS: ESC SPI link OK (read 0x87654321)\n");
            return 0;
        }
        if (rc != 0)
        {
            kos::print("[k64dspi] LAN9252 BYTE_TEST FAIL: a transfer failed\n");
            return 1;
        }
        kos::print("[k64dspi] LAN9252 BYTE_TEST MISMATCH: no valid signature; check CS "
                   "(D9/PTC4), baud/mode, or shield seating\n");
        return 1;
    }

#endif
}

extern "C" void k64dspi_main(kos_self_t const* self)
{
    struct kos_spi_bus_config bcfg;
    bcfg.irq = KOS_CAP_NONE; // the DSPI pump polls its FIFOs
    bcfg.notify = KOS_CAP_NONE;
    bcfg.notify_bit = 0;
    bcfg.irq_index = 0u;
#if KICKOS_SPI_LOCAL
    kos_window_t const window = kos_grant_mmio(self, "/dev/dspi0");
    uintptr_t const win = reinterpret_cast<uintptr_t>(kos_window_addr(window));
    if (win == 0u or kos_window_size(window) < kickos::mk64f::mmap::DSPI0_SIZE)
    {
        kos::print("[k64dspi] ERROR: no /dev/dspi0 window\n");
        exit(1);
    }
    int32_t const muxed = k64dspi_bus_mux(win);
    if (muxed != 0)
    {
        char e[64];
        ksnprintf(e, sizeof(e), "[k64dspi] ERROR: bus pins rc %d\n", static_cast<int>(muxed));
        kos::print(e);
        exit(1);
    }
    bcfg.base = win;
    bcfg.ep = KOS_CAP_NONE;
#else
    kos_cap_t const ep = kos_grant_endpoint(self, "/svc/spi0");
    if (ep == KOS_CAP_NONE)
    {
        kos::print("[k64dspi] ERROR: no /svc/spi0 endpoint\n");
        exit(1);
    }
    bcfg.base = 0u;
    bcfg.ep = ep;
#endif
    exit(run_client(&bcfg));
}
