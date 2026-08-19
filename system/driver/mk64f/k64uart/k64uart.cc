// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/UART0 userspace polled UART TX console driver (see <kickos/driver/k64uart.h>).
//
// The driver touches no SIM or PORT register: the kernel's uart0_init() configured clock
// and pins at boot and left the UART TX-capable.
//
// HARD RULE (design D7): NO libc stdio here. printf/puts route through _write ->
// kos_send(0, ..) -> this driver's own endpoint, a self-send that deadlocks because the
// driver holds the sole CAP_WAIT recv cap. Diagnostics go direct to the device (win_puts)
// or via kos::print.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/k64uart.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/service.h> // kos_service_cfg

#include <stdint.h>
#include <stdlib.h>

namespace drv = kickos::driver;

namespace
{
    // Per-byte cap on the retry, so a mis-configured baud/enable never HANGS the driver
    // thread on a single byte, which would wedge every stdout client parked on send. On
    // expiry the byte is dropped and the loop continues.
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

    constexpr drv::Descriptor k_desc = {
        .tag = "[k64uart] ",
        // No base guard: no vector is claimed by number and uart_k64.cc is genuinely
        // base-parameterised across the five UART instances, so there is nothing to pin
        // the cfg against.
        .expected_base = 0,
        .block_size = 0, // polled and TX-only: no ring, no doorbell, no readiness latch
        .block_flags = 0,
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
        // Removing the window grant kills the console even though SYSMPU cannot gate a
        // peripheral: possession is the sole authorisation for the kos_periph_enable
        // inside kos_uart_open (docs/reference/boards.md, "When an MMIO grant is INERT").
        .threads = {{.entry = k64uart_console_driver,
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
                  "the k64uart descriptor is not a well-formed driver shape");
}

extern "C"
{

void k64uart_console_driver(void* arg)
{
    uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // UART0 window base

    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = {};
    cfg.base = win;
    cfg.stats = &stats;
    cfg.baud = 0; // keep the kernel's divisor; open reports what it reads back
    cfg.data_bits = 8;
    cfg.parity = KOS_UART_PARITY_NONE;
    cfg.stop_bits = 1;
    cfg.rsv = 0;

    // NO TRANSPORT SURVIVES A FAILURE HERE: the console is already USER_OWNED, so the line
    // below reaches the wire only on a build carrying RTT. This thread's stdout cap is the
    // endpoint it was spawned to SERVE, and the window is supervisor-only until
    // kos_uart_open's AIPS release, so neither a send nor win_puts can report either.
    struct kos_uart dev;
    if (kos_uart_open(&dev, &cfg) < 0)
    {
        kos::print("[k64uart] ERROR: UART0 open refused, device unreachable\n");
        exit(-1);
    }

    win_puts(&dev, "[k64uart] driver up (polled TX)\n");

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

    // FLUSH BEFORE CLOSE: close stops the transmitter, which truncates a frame still
    // shifting.
    (void)kos_uart_flush(&dev);
    (void)kos_uart_close(&dev);
    exit(0);
}

// cfg->prio must be >= every stdout client (D9: rendezvous has no PI).
int k64uart_console_start(struct kos_service_cfg const* cfg)
{
    return drv::bring_up(k_desc, cfg, nullptr);
}

}
