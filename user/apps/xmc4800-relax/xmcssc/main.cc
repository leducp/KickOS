// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800/USIC0-CH1 SSC bus-SERVICE silicon validation (M4.4 call/reply). The SSC
// service is brought up by the board service list BEFORE main (xmc_spi0_start: endpoint
// + unprivileged driver, which configures the channel itself). This app only drives it
// as a CLIENT -- the XMC sibling of user/apps/k64dspi.
//
// Because a kos_call caller must be a spawned pool thread (the root/init thread is
// guarded -> -KOS_EPERM), the client is a SPAWNED thread that receives the service
// endpoint's SIGNAL cap by spawn-time delegation (positional: child index 1). main
// (running in the root/init thread, sharing its cap table) reads the endpoint handle
// the service recorded (xmc_spi0_take_endpoint, a one-shot handout) and delegates a
// SIGNAL-narrowed copy, then
// closes its own retained cap so the driver is the sole recv holder (driver death
// then EPIPE-wakes any parked client). The client speaks the neutral wrapper
// (spi_transfer / spi_transact / spi_config over the bus call/reply ABI); it touches
// no MMIO, no CS, no grant -- the driver owns all of that.
//
// Data path is the driver's INTERNAL LOOP-BACK (DX0 = own transmitter): there is no
// external XMC SPI device on the bench, so every byte echoes (rx == tx). The client
// configures KOS_BUS_CS_HW so the driver exercises the FEM=1 MSLS/SELO0 CS-hold
// framing (the coherent transaction) across every transfer.
//
// Diagnostic app (kickos_add_diagnostic_app): build-only; the operator flashes +
// validates on silicon. No CTest gate (it answers a HARDWARE question).

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <kickos/driver/xmcssc.h>     // xmc_spi0_take_endpoint()
#include <kickos/driver/spi_client.h> // spi_transfer / spi_transact / spi_config

#include <stdint.h>
#include <stddef.h>

// Backstops the CMake enforcement-build gate: the MMIO-grant seam is an M2 construct,
// and a granted DEV window is only a real capability under PMSA.
#if !KICKOS_HAVE_MPU
#error "xmcssc requires the enforcement build: configure with -DKICKOS_HAVE_MPU=1"
#endif

namespace
{
    // The delegated SPI service endpoint SIGNAL cap lands at the client's child table
    // index 1 (positional spawn delegation).
    constexpr int SPI_EP = 1;

    // The single device on the bench's bus. A client with several devices gives each
    // its own slot (< KOS_BUS_DEV_MAX) and configures each once.
    constexpr uint8_t SPI_DEV = 0;

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

    // UNPRIVILEGED client: CONFIG (HW CS so the driver runs the FEM=1 CS-hold framing)
    // then loopback transfers through the neutral wrapper. SIGNAL cap at index 1.
    void spi_client(void*)
    {
        struct kos_bus_cfg cfg = {};
        cfg.hz = 1000000u; // informational: the XMC baud profile is fixed at bring-up
        cfg.mode = 0u;      // SPI mode 0 (CPOL/CPHA are fixed at bring-up on XMC)
        cfg.word_bits = 8u;
        cfg.cs_policy = KOS_BUS_CS_HW; // hardware MSLS/SELO0, held across the frame
        uint32_t achieved = 0u;
        int const crc = spi_config(SPI_EP, SPI_DEV, &cfg, &achieved);
        {
            char s[80];
            ksnprintf(s, sizeof(s), "[xmcssc] config rc=%d achieved=%lu Hz\n", crc,
                      static_cast<unsigned long>(achieved));
            kos::print(s);
        }
        report("config", crc == 0);

        // 1) Single bytes echo through the on-chip loopback.
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

        // 2) Multi-byte transfer (exercises the per-word IRQ-paced refill loop and, for
        //    CS_HW, the SOF..EOF frame spanning all words in one MSLS bracket).
        {
            unsigned char tx[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
            unsigned char rx[5] = {0};
            long n = spi_transfer(SPI_EP, SPI_DEV, tx, rx, sizeof(tx));
            report("multi-byte loopback",
                   n == static_cast<long>(sizeof(tx)) and buffers_equal(tx, rx, sizeof(tx)));
        }

        // 3) Null tx: the wrapper shifts dummy 0x00, so the loopback returns 0x00.
        {
            unsigned char rx[4] = {0xAAu, 0xAAu, 0xAAu, 0xAAu};
            long n = spi_transfer(SPI_EP, SPI_DEV, nullptr, rx, sizeof(rx));
            report("null-tx (dummy 0x00) loopback",
                   n == static_cast<long>(sizeof(rx)) and buffer_is(rx, 0x00u, sizeof(rx)));
        }

        // 4) Two-phase transact in one CS bracket (cmd+payload shape). Over the loopback
        //    the read phase shifts dummy 0x00 out -> reads back 0x00; the whole
        //    transaction is one FEM-held MSLS frame.
        {
            unsigned char cmd[3] = {0x03u, 0x00u, 0x64u};
            unsigned char rd[4] = {0};
            long n = spi_transact(SPI_EP, SPI_DEV, cmd, sizeof(cmd), rd, sizeof(rd));
            report("transact (cmd+read, one CS bracket)",
                   n == static_cast<long>(sizeof(rd)) and buffer_is(rd, 0x00u, sizeof(rd)));
        }

        if (g_fails == 0)
        {
            kos::print("[xmcssc] loopback PASS (call/reply SSC service echoes tx==rx)\n");
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
    // The board service list already brought the SSC service up (privileged USIC
    // config + endpoint + unprivileged driver) before this main. Take the endpoint it
    // recorded.
    int const ep = xmc_spi0_take_endpoint();
    if (ep < 0)
    {
        kos::print("[xmcssc] ERROR: SPI service not up (endpoint unavailable)\n");
    }
    else
    {
        // Delegate a SIGNAL-narrowed copy of E to the spawned client (child index 1).
        // The client is the caller (a pool thread); root cannot kos_call.
        kos_cap_grant const caps[1] = {
            { /*source_cap=*/ep, /*rights_mask=*/KOS_CAP_SIGNAL },
        };
        int const c = kos::thread::spawn_caps(spi_client, nullptr, "xmcssc-cli", 9,
                                              caps, /*cap_count=*/1);
        if (c < 0)
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

    // Park: fall back to a sleep park if the idle semaphore could not be created.
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
