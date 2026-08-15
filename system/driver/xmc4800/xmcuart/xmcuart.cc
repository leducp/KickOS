// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 userspace polled UART TX console driver (see <kickos/driver/xmcuart.h>).
//
// The driver does NOT program clock or pins: the kernel's kickos_xmc_usic_init() configured
// them at boot and console_tx_deinit() left the channel ASC-mode, pinned and TX-capable.
//
// HARD RULE (design D7): NO libc stdio here. printf/puts route through _write ->
// kos_send(0, ..) -> this driver's own endpoint, a self-send that deadlocks because the
// driver holds the sole CAP_WAIT recv cap, so no EPIPE ever fires. Diagnostics go direct to
// the device (win_puts) or via kos::print.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/xmcuart.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/service.h> // kos_service_cfg

#include <stdint.h>
#include <stdlib.h>

namespace drv = kickos::driver;

namespace
{
    // Per-byte cap on the retry, so a channel that never reports room costs a bounded delay
    // rather than the driver thread, which would wedge every stdout client parked on send.
    // On expiry the byte is dropped and the loop continues.
    constexpr uint32_t TX_POLL_TIMEOUT = 1000000u;

    // Returns false when the budget expired with the byte still unsent.
    bool poll_put(struct kos_uart* dev, unsigned char v)
    {
        for (uint32_t i = 0; i < TX_POLL_TIMEOUT; i++)
        {
            if (kos_uart_write(dev, &v, 1u) == 1u)
            {
                return true;
            }
        }
        return false;
    }

    // Direct-to-device diagnostic, not stdio and not the endpoint.
    void win_puts(struct kos_uart* dev, char const* s)
    {
        for (; *s != '\0'; s++)
        {
            (void)poll_put(dev, static_cast<unsigned char>(*s));
        }
    }

    void print_rate(char const* tag, uint32_t hz)
    {
        char buf[16];
        size_t i = sizeof(buf);
        buf[--i] = '\0';
        uint32_t v = hz;
        do
        {
            buf[--i] = static_cast<char>('0' + (v % 10u));
            v /= 10u;
        } while (v != 0u and i != 0);
        kos::print(tag);
        kos::print(&buf[i]);
        kos::print("\n");
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[xmcuart] ",
        // No base guard: no vector is claimed by number and uart_usic.cc is genuinely
        // base-parameterised across the USIC channels, so there is nothing to pin the cfg
        // against.
        .expected_base = 0,
        .block_size = 0, // polled and TX-only: no ring, no doorbell, no readiness latch
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 0,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        // ONE thread, so no readiness latch: it is itself the endpoint's receiver, and no
        // point exists before it at which a timeout would be reportable. It also releases
        // the window at its own death, which is what lets the console come back.
        //
        // On ARMv7-M PMSA the 512 B (0x200) window at the 0x200-aligned channel base is
        // one exact-cover descriptor, leaving the sibling channel U0C1 (base + 0x200) and
        // the SCU/IOCR peripherals outside it.
        .threads = {{.entry = xmcuart_console_driver,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_WINDOW,
                     .window_grant = true,
                     .cap_count = 1,
                     // caps[0] lands at KOS_SPAWN_DELEGATED_CAP0, which the recv loop
                     // below names directly; no class substrate checks it.
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc),
                  "the xmcuart descriptor is not a well-formed driver shape");
}

extern "C"
{

void xmcuart_console_driver(void* arg)
{
    uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // U0C0 window base

    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = {};
    cfg.base = win;
    cfg.stats = &stats;
    cfg.baud = 0; // keep the kernel's divisor; open reports what it reads back
    cfg.data_bits = 8;
    cfg.parity = KOS_UART_PARITY_NONE;
    cfg.stop_bits = 1;
    cfg.rsv = 0;

    struct kos_uart dev;
    int32_t const rate = kos_uart_open(&dev, &cfg);
    if (rate < 0)
    {
        // kos::print, not the endpoint: this thread's stdout cap IS the console endpoint it
        // was spawned to serve, so a send would park on an endpoint with no receiver.
        kos::print("[xmcuart] ERROR: U0C0 open refused\n");
        exit(-1);
    }
    print_rate("[xmcuart] U0C0 measured baud: ", static_cast<uint32_t>(rate));

    win_puts(&dev, "[xmcuart] driver up (polled TX)\n");

    int const ep = KOS_SPAWN_DELEGATED_CAP0; // delegated recv cap
    char buf[KOS_EP_MSG_MAX];
    while (true)
    {
        // Info-less recv: the console hosts plain sends only, so a client kos_call
        // bounces cleanly (-KOS_ENOSYS) instead of minting a reply cap here.
        long const n = kos_recv(ep, buf, sizeof(buf), nullptr);
        if (n < 0)
        {
            // Endpoint dead / EPIPE or a bad cap: unrecoverable. Exit and let root respawn
            // + re-publish (D8). Do NOT diagnose via stdio here.
            break;
        }
        for (long i = 0; i < n; i++)
        {
            (void)poll_put(&dev, static_cast<unsigned char>(buf[i]));
        }
    }

    // FLUSH BEFORE CLOSE: close leaves ASC mode, which truncates a frame still shifting.
    (void)kos_uart_flush(&dev);
    (void)kos_uart_close(&dev);
    exit(0);
}

// cfg->prio must be >= every stdout client (D9: rendezvous has no PI).
int xmcuart_console_start(struct kos_service_cfg const* cfg)
{
    return drv::bring_up(k_desc, cfg, nullptr);
}

}
