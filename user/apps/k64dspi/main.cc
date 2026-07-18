// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/DSPI0 SPI transport silicon validation (task #9 Stage 3 -> KickCAT Stage C).
// Two build modes over the SAME callable blocking transport (spi_transfer /
// spi_enable_cs / spi_disable_cs, see <kickos/driver/k64dspi.h>):
//
//   DEFAULT -- LAN9252 BYTE_TEST probe. An EasyCAT LAN9252 shield is on the Arduino
//   R3 header; the probe reads the ESC's byte-order signature register (0x0064) and
//   confirms it reads 0x8765_4321. This validates the transport against a REAL
//   EtherCAT slave controller: real MISO data, CS framing, and real-ESC baud/mode --
//   which the loopback cannot exercise.
//
//   K64DSPI_LOOPBACK -- the original SOUT(PTD2)->SIN(PTD3) loopback self-test, for
//   when the shield is off and the PTD2<->PTD3 jumper is in place. Kept behind the
//   flag (CMake option K64DSPI_LOOPBACK=ON) so it is not lost.
//
// Both spawn a SEPARATE unprivileged CLIENT thread that drives the transport -- the
// exact cross-thread request/reply a KickCAT slave thread uses. Their tx/rx buffers
// live on a PRIVATE stack unreachable by the driver thread's domain, which is why the
// transport bounce-copies them in the client's context.
//
// K64F peripheral privilege is AIPS-PACR, NOT SYSMPU: once slot 44 is opened every
// unprivileged thread reaches DSPI0 (no per-thread peripheral boundary on K64F). The
// MMIO grant is inert for the peripheral -- documented, unchanged. A PASS proves the
// UNPRIVILEGED transport works over the AIPS-opened slot (kernel-vs-user isolation);
// per-thread peripheral isolation is the F411/PMSA case (user/apps/f411spi).
//
// Diagnostic app (kickos_add_diagnostic_app): never a production image. Build-only;
// the operator flashes + validates on silicon.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <kickos/driver/k64dspi.h>

#include <stdint.h>
#include <stddef.h>

// Backstops the CMake enforcement-build gate: the MMIO-grant seam is an M2 construct.
#if !KICKOS_HAVE_MPU
#error "k64dspi requires the enforcement build: configure with -DKICKOS_HAVE_MPU=1"
#endif

#if defined(K64DSPI_LOOPBACK)

namespace
{
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

    // UNPRIVILEGED client: the stand-in for a KickCAT slave thread. Its tx/rx buffers
    // live on its PRIVATE stack -- unreachable by the driver thread's domain -- which
    // is exactly why the transport bounce-copies them in this thread's context.
    void spi_client(void*)
    {
        // 1) Single bytes echo through the loopback.
        {
            unsigned char const pattern[] = {0xA5u, 0x3Cu, 0x00u, 0xFFu};
            bool ok = true;
            for (unsigned i = 0; i < sizeof(pattern); i++)
            {
                unsigned char tx = pattern[i];
                unsigned char rx = 0;
                int n = spi_transfer(&tx, &rx, 1);
                if (n != 1 or rx != tx)
                {
                    ok = false;
                }
            }
            report("single-byte loopback", ok);
        }

        // 2) Multi-byte transfer LARGER than the TX FIFO (exercises batching + the
        // per-batch EOQ wake). Over the loopback the whole buffer must echo.
        {
            unsigned char tx[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
            unsigned char rx[5] = {0};
            int n = spi_transfer(tx, rx, sizeof(tx));
            report("multi-byte (>FIFO) loopback",
                   n == static_cast<int>(sizeof(tx)) and buffers_equal(tx, rx, sizeof(tx)));
        }

        // 3) Null tx: the driver shifts dummy 0x00, so the loopback returns 0x00.
        {
            unsigned char rx[4] = {0xAAu, 0xAAu, 0xAAu, 0xAAu};
            int n = spi_transfer(nullptr, rx, sizeof(rx));
            report("null-tx (dummy 0x00) loopback",
                   n == static_cast<int>(sizeof(rx)) and buffer_is(rx, 0x00u, sizeof(rx)));
        }

        // 4) Null rx: write-only, received bytes discarded; must not fault/hang.
        {
            unsigned char tx[4] = {0xDEu, 0xADu, 0xBEu, 0xEFu};
            int n = spi_transfer(tx, nullptr, sizeof(tx));
            report("null-rx (discard) transfer", n == static_cast<int>(sizeof(tx)));
        }

        // 5) CS-held header+payload bracket (LAN9252 CSR shape): enable -> 3-byte
        // "cmd" -> payload -> disable. Data still echoes over the loopback; this
        // proves the CONT path + release handshake do not hang. (CS timing = bench.)
        {
            unsigned char cmd[3] = {0x03u, 0x00u, 0x64u};
            unsigned char rx_cmd[3] = {0};
            unsigned char payload[4] = {0};
            unsigned char rx_payload[4] = {0};

            spi_enable_cs();
            int n1 = spi_transfer(cmd, rx_cmd, sizeof(cmd));
            int n2 = spi_transfer(payload, rx_payload, sizeof(payload));
            spi_disable_cs();

            bool ok = true;
            if (n1 != static_cast<int>(sizeof(cmd)) or not buffers_equal(cmd, rx_cmd, sizeof(cmd)))
            {
                ok = false;
            }
            if (n2 != static_cast<int>(sizeof(payload)) or not buffers_equal(payload, rx_payload, sizeof(payload)))
            {
                ok = false;
            }
            report("CS-held bracket (cmd+payload)", ok);
        }

        if (g_fails == 0)
        {
            kos::print("[k64dspi] loopback PASS (transport echoes tx==rx through the "
                       "blocking API + CS-hold)\n");
        }
        else
        {
            kos::print("[k64dspi] loopback FAIL (see per-case lines above)\n");
        }

        kos::print("[k64dspi] unprivileged DSPI transport works over AIPS-opened slot 44 "
                   "(kernel-vs-user isolation; per-thread peripheral isolation = F411/PMSA)\n");

        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

#else // LAN9252 BYTE_TEST probe (default)

namespace
{
    // LAN9252 register READ framing (Microchip LAN9252 datasheet, SPI slave):
    // cmd byte 0x03, then the 16-bit register address big-endian, then read N data
    // bytes. BYTE_TEST (0x0064) reads back 0x8765_4321 -- the byte-order signature.
    constexpr uint32_t LAN9252_BYTE_TEST = 0x87654321u;
    constexpr uint16_t BYTE_TEST_ADDR = 0x0064u;
    constexpr unsigned char LAN9252_READ = 0x03u;
    constexpr int PROBE_RETRIES = 8;
    constexpr uint64_t RETRY_DELAY_NS = 10000000ull; // 10 ms ESC settle between tries

    // One BYTE_TEST read over the Stage-C transport: CS held across the 3-byte command
    // and the 4 data bytes, released after. Bytes arrive LSB-first, so assemble
    // little-endian. Sets *ok to the transfer success and returns the 32-bit value.
    uint32_t read_byte_test(bool* ok)
    {
        unsigned char cmd[3];
        cmd[0] = LAN9252_READ;
        cmd[1] = static_cast<unsigned char>((BYTE_TEST_ADDR >> 8) & 0xFFu);
        cmd[2] = static_cast<unsigned char>(BYTE_TEST_ADDR & 0xFFu);

        unsigned char rx[4] = {0, 0, 0, 0};

        spi_enable_cs();
        int n1 = spi_transfer(cmd, nullptr, sizeof(cmd));
        int n2 = spi_transfer(nullptr, rx, sizeof(rx));
        spi_disable_cs();

        *ok = (n1 == static_cast<int>(sizeof(cmd)) and n2 == static_cast<int>(sizeof(rx)));

        uint32_t val = static_cast<uint32_t>(rx[0]);
        val |= static_cast<uint32_t>(rx[1]) << 8;
        val |= static_cast<uint32_t>(rx[2]) << 16;
        val |= static_cast<uint32_t>(rx[3]) << 24;
        return val;
    }

    // UNPRIVILEGED client: the stand-in for a KickCAT slave thread driving a real ESC.
    void spi_probe(void*)
    {
        bool pass = false;
        for (int attempt = 1; attempt <= PROBE_RETRIES and not pass; attempt++)
        {
            bool ok = false;
            uint32_t val = read_byte_test(&ok);

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
                       "(read 0x87654321 through the unprivileged Stage-C transport)\n");
        }
        else
        {
            kos::print("[k64dspi] LAN9252 BYTE_TEST FAIL: no valid signature -- check CS "
                       "(D9/PTC4), baud/mode, or shield seating (0xffffffff=MISO high/"
                       "no drive, 0x0=MISO low/held)\n");
        }

        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

#endif

int main(int, char**)
{
#if defined(K64DSPI_LOOPBACK)
    int const loopback = 1;
#else
    int const loopback = 0;
#endif

    if (spi_driver_start(loopback) < 0)
    {
        kos::print("[k64dspi] ERROR: transport start failed\n");
    }
    else
    {
#if defined(K64DSPI_LOOPBACK)
        int c = kos::thread::spawn(spi_client, nullptr, "k64spi-cli", 9,
                                   KOS_POLICY_FIFO, 0, /*privileged=*/false);
#else
        int c = kos::thread::spawn(spi_probe, nullptr, "k64lan-prb", 9,
                                   KOS_POLICY_FIFO, 0, /*privileged=*/false);
#endif
        if (c < 0)
        {
            kos::print("[k64dspi] ERROR: client spawn failed\n");
        }
    }

    // Park: fall back to a sleep park if the idle semaphore could not be created
    // (else a -1 handle spins a hot loop of failing sem_wait syscalls).
    int idle = kos_sem_create(0);
    while (true)
    {
        if (idle < 0)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
