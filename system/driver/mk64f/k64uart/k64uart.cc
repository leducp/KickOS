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

#include <kickos/driver/declared/k64uart.h>
#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart_service.h>

#include <stdint.h>
#include <stdlib.h>

namespace drv = kickos::driver;
namespace uart = kickos::uart;
namespace declared = kickos::driver::declared::k64uart;

namespace
{
    constexpr drv::Descriptor k_desc = {
        .tag = "[k64uart] ",
        // No base guard: uart_k64.cc is base-parameterised across the five UART instances.
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
        // Removing the window grant kills the console even though SYSMPU cannot gate a
        // peripheral: possession is the sole authorisation for the kos_periph_enable
        // inside kos_uart_open.
        .threads = {{.entry = k64uart_console_driver,
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
                  "the k64uart descriptor is not a well-formed driver shape");
    static_assert(drv::declared_as(k_desc, declared::k_declared),
                  "the k64uart descriptor departs from its kickos_add_driver declaration");
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

    uart::win_puts(&dev, "[k64uart] driver up (polled TX)\n");

    uart::polled_console_loop(&dev);
    exit(0);
}

// The task's priority must be >= every stdout client's (D9: rendezvous has no PI).
int k64uart_console_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}

}
