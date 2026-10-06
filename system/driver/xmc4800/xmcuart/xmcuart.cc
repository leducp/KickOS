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

#include <kickos/driver/declared/xmcuart.h>
#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart_service.h>

#include <stdint.h>
#include <stdlib.h>

namespace drv = kickos::driver;
namespace uart = kickos::uart;
namespace declared = kickos::driver::declared::xmcuart;

namespace
{
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
        // No base guard: uart_usic.cc is base-parameterised across the USIC channels.
        .expected_base = 0,
        .block_size = declared::k_declared.block_size, // polled and TX-only: no ring, doorbell or latch
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = declared::k_declared.ep_posture,
        .line_count = declared::k_declared.line_count,
        .thread_count = declared::k_declared.thread_count,
        .barrier_after = declared::k_declared.barrier_after,
        .lines = {},
        // ONE thread, so no readiness latch: it is itself the endpoint's receiver, and no
        // point exists before it at which a timeout would be reportable. It also releases
        // the window at its own death, which is what lets the console come back.
        //
        // On ARMv7-M PMSA the 512 B (0x200) window at the 0x200-aligned channel base is
        // one exact-cover descriptor, leaving the sibling channel U0C1 (base + 0x200) and
        // the SCU/IOCR peripherals outside it.
        .threads = {{.entry = xmcuart_console_driver,
                     .name = declared::k_declared.thread_name[0],
                     .prio_delta = declared::k_declared.prio_delta[0],
                     .arg = drv::KOS_DRV_ARG_WINDOW,
                     .window_grant = true,
                     .cap_count = 1,
                     // caps[0] lands at KOS_SPAWN_DELEGATED_CAP0, which
                     // uart::polled_console_loop receives on; no class substrate checks it.
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc),
                  "the xmcuart descriptor is not a well-formed driver shape");
    static_assert(drv::declared_as(k_desc, declared::k_declared),
                  "the xmcuart descriptor departs from its kickos_add_driver declaration");
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

    uart::win_puts(&dev, "[xmcuart] driver up (polled TX)\n");

    uart::polled_console_loop(&dev);
    exit(0);
}

// The task's priority must be >= every stdout client's (D9: rendezvous has no PI).
int xmcuart_console_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}

}
