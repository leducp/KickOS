// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/DSPI0 SPI bus-SERVICE silicon validation (call/reply). The DSPI0 service
// is brought up by the board service list BEFORE main (k64dspi_spi_start: privileged
// DSPI config + endpoint + unprivileged driver). This app only drives it as a CLIENT.
//
// Because a kos_call caller must be a spawned pool thread (the root/init thread is
// guarded -> -KOS_EPERM), the client is a SPAWNED thread that receives the service
// endpoint's SIGNAL cap by spawn-time delegation (positional: child index 1). main
// (running in the root/init thread, sharing its cap table) reads the endpoint handle
// the service recorded (k64dspi_take_endpoint, a one-shot handout) and delegates a
// SIGNAL-narrowed copy, then
// closes its own retained cap so the driver is the sole recv holder (driver death
// then EPIPE-wakes any parked client). The client speaks the neutral wrapper
// (spi_transfer / spi_transact / spi_config over the bus call/reply ABI); it touches
// no MMIO, no CS, no grant; the driver owns all of that.
//
// Two build modes over the SAME service:
//   DEFAULT: LAN9252 BYTE_TEST probe (EasyCAT shield on the Arduino header): a
//   spi_transact(cmd, rx) reads the ESC byte-order signature 0x8765_4321.
//   K64DSPI_LOOPBACK: SOUT(PTD2)->SIN(PTD3) loopback self-test (jumper, no shield),
//   behind CMake option K64DSPI_LOOPBACK=ON.
//
// Diagnostic app (kickos_add_diagnostic_app): build-only; the operator flashes +
// validates on silicon. No CTest gate (it answers a HARDWARE question).

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <kickos/driver/k64dspi.h>    // k64dspi_take_endpoint()
#include <kickos/driver/spi_client.h> // spi_transfer / spi_transact / spi_config

#include <stdint.h>
#include <stddef.h>

// Backstops the CMake enforcement-build gate: the MMIO-grant seam needs enforcement.
#if !KICKOS_HAVE_MPU
#error "k64dspi requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // The delegated SPI service endpoint SIGNAL cap lands at the client's child
    // table index 1 (positional spawn delegation).
    constexpr int SPI_EP = 1;

    // The single device on the bench's bus. A client with several devices gives each
    // its own slot (< KOS_BUS_DEV_MAX) and configures each once.
    constexpr uint8_t SPI_DEV = 0;

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

    // UNPRIVILEGED client: CONFIG (conservative baud, no CS; the loopback jumper
    // has none) then loopback transfers through the wrapper. SIGNAL cap at index 1.
    void spi_client(void*)
    {
        struct kos_bus_cfg cfg = {};
        cfg.hz = 1000000u; // conservative over a jumper
        cfg.mode = 0u;      // SPI mode 0
        cfg.word_bits = 8u;
        cfg.cs_policy = KOS_BUS_CS_NONE;
        uint32_t achieved = 0u;
        int const crc = spi_config(SPI_EP, SPI_DEV, &cfg, &achieved);
        {
            char s[80];
            ksnprintf(s, sizeof(s), "[k64dspi] config rc=%d achieved=%lu Hz\n", crc,
                      static_cast<unsigned long>(achieved));
            kos::print(s);
        }
        report("config", crc == 0);

        // 1) Single bytes echo through the loopback.
        {
            unsigned char const pattern[] = {0xA5u, 0x3Cu, 0x00u, 0xFFu};
            bool ok = true;
            for (unsigned i = 0; i < sizeof(pattern); i++)
            {
                unsigned char tx = pattern[i];
                unsigned char rx = 0;
                long n = spi_transfer(SPI_EP, SPI_DEV, &tx, &rx, 1);
                if (n != 1 or rx != tx)
                {
                    ok = false;
                }
            }
            report("single-byte loopback", ok);
        }

        // 2) Multi-byte transfer larger than the TX FIFO (exercises the refill loop).
        {
            unsigned char tx[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
            unsigned char rx[5] = {0};
            long n = spi_transfer(SPI_EP, SPI_DEV, tx, rx, sizeof(tx));
            report("multi-byte (>FIFO) loopback",
                   n == static_cast<long>(sizeof(tx)) and buffers_equal(tx, rx, sizeof(tx)));
        }

        // 3) Null tx: the driver shifts dummy 0x00, so the loopback returns 0x00.
        {
            unsigned char rx[4] = {0xAAu, 0xAAu, 0xAAu, 0xAAu};
            long n = spi_transfer(SPI_EP, SPI_DEV, nullptr, rx, sizeof(rx));
            report("null-tx (dummy 0x00) loopback",
                   n == static_cast<long>(sizeof(rx)) and buffer_is(rx, 0x00u, sizeof(rx)));
        }

        // 4) Two-phase transact in one CS bracket (cmd+payload shape). Over the
        //    loopback both phases echo; the wrapper returns only the read-phase bytes.
        {
            unsigned char cmd[3] = {0x03u, 0x00u, 0x64u};
            unsigned char rd[4] = {0};
            long n = spi_transact(SPI_EP, SPI_DEV, cmd, sizeof(cmd), rd, sizeof(rd));
            // Read phase shifts dummy 0x00 out over the loopback -> reads back 0x00.
            report("transact (cmd+read, one CS bracket)",
                   n == static_cast<long>(sizeof(rd)) and buffer_is(rd, 0x00u, sizeof(rd)));
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

    // One BYTE_TEST read: cmd (0x03 + 16-bit addr big-endian) then 4 read bytes, all
    // under ONE CS bracket (the driver holds PTC4 across both phases). Bytes arrive
    // LSB-first -> assemble little-endian. *ok gets the transfer success.
    uint32_t read_byte_test(bool* ok)
    {
        unsigned char cmd[3];
        cmd[0] = LAN9252_READ;
        cmd[1] = static_cast<unsigned char>((BYTE_TEST_ADDR >> 8) & 0xFFu);
        cmd[2] = static_cast<unsigned char>(BYTE_TEST_ADDR & 0xFFu);

        unsigned char rx[4] = {0, 0, 0, 0};
        long n = spi_transact(SPI_EP, SPI_DEV, cmd, sizeof(cmd), rx, sizeof(rx));
        *ok = (n == static_cast<long>(sizeof(rx)));

        uint32_t val = static_cast<uint32_t>(rx[0]);
        val |= static_cast<uint32_t>(rx[1]) << 8;
        val |= static_cast<uint32_t>(rx[2]) << 16;
        val |= static_cast<uint32_t>(rx[3]) << 24;
        return val;
    }

    // UNPRIVILEGED client: CONFIG (10 MHz, GPIO CS) then the BYTE_TEST probe.
    void spi_client(void*)
    {
        struct kos_bus_cfg cfg = {};
        cfg.hz = 10000000u;
        cfg.mode = 0u;
        cfg.word_bits = 8u;
        cfg.cs_policy = KOS_BUS_CS_GPIO;
        cfg.cs_index = 4u; // PTC4 by intent; the driver ignores it and always drives PTC4
        uint32_t achieved = 0u;
        int const crc = spi_config(SPI_EP, SPI_DEV, &cfg, &achieved);
        {
            char s[80];
            ksnprintf(s, sizeof(s), "[k64dspi] config rc=%d achieved=%lu Hz\n", crc,
                      static_cast<unsigned long>(achieved));
            kos::print(s);
        }

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
                       "(read 0x87654321 through the call/reply SPI service)\n");
        }
        else
        {
            kos::print("[k64dspi] LAN9252 BYTE_TEST FAIL: no valid signature -- check CS "
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
    // The board service list already brought DSPI0 up (privileged config + endpoint
    // + unprivileged driver) before this main. Take the endpoint it recorded.
    kos_cap_t const ep = k64dspi_take_endpoint();
    if (ep == KOS_CAP_NONE)
    {
        kos::print("[k64dspi] ERROR: SPI service not up (endpoint unavailable)\n");
    }
    else
    {
        // Delegate a SIGNAL-narrowed copy of E to the spawned client (child index 1).
        // The client is the caller (a pool thread); root cannot kos_call.
        kos_cap_grant const caps[1] = {
            { /*source_cap=*/ep, /*rights_mask=*/KOS_CAP_SIGNAL },
        };
        auto const c = kos::thread::spawn_caps(spi_client, nullptr, "k64spi-cli", 9,
                                               caps, /*cap_count=*/1);
        if (not c.valid())
        {
            kos::print("[k64dspi] ERROR: client spawn failed\n");
        }
        else
        {
            // Drop root's own cap so the driver is the sole recv holder: its death
            // then EPIPE-wakes the client instead of leaving it parked (S4-style).
            kos_handle_close(ep);
        }
    }

    // Park: fall back to a sleep park if the idle semaphore could not be created.
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
