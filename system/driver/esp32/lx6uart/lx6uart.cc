// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32 (Xtensa LX6) UART0 buffered userspace UART driver.
//
// The console line cannot be claimed while the kernel's own TX ring holds it, so the publish
// MUST precede the claim: that ordering is KOS_DRV_EP_HANDOVER's.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/service.h>
#include <kickos/sys/uart_service.h>

#include "irq.h"
#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace drv = kickos::driver;
namespace uart = kickos::uart;
namespace mmap = kickos::esp32::mmap;
namespace lx6irq = kickos::esp32::irq;

namespace
{
    constexpr uart::UartParams k_uart = {
        .open_fail = "[lx6uart] UART0 open refused: divisor read-back or frame",
        .announce = "[lx6uart] device up (IRQ TX/RX)\n",
        .prime = true
    };

    // No kos_periph_enable and no DPORT clock ungate: the ROM ran its boot log through
    // UART0, so the block is already clocked out of reset.
    void irq_entry(void* arg)
    {
        uart::irq_thread<struct kos_uart>(static_cast<uart::Ctx*>(arg), k_uart);
    }

    int block_init(void* blk, struct kos_service_cfg const* cfg)
    {
        // hz travels as the REQUESTED baud; 0 keeps the divisor the ROM left. The frame is
        // Register 19.9's reset framing, so the backend has nothing to reprogram.
        return uart::ctx_init(static_cast<uart::Ctx*>(blk), cfg, /*fallback_baud=*/0u);
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[lx6uart] ",
        .expected_base = mmap::UART0_BASE,
        .block_size = uart::KOS_UART_BLOCK_SIZE,
        .ready_offset = uart::KOS_UART_READY_OFFSET,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 1,
        .thread_count = 2,
        .barrier_after = 1,
        // LEVEL: CPU interrupt 13 is level-triggered (TRM Table 8.3-2) and the UART latch
        // stays set until the driver clears it.
        .lines = {{lx6irq::CONSOLE_TX_LINE, KOS_IRQ_LEVEL}},
        .threads = {{.entry = irq_entry,
                     .name = "lx6uartirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = true,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT}}},
                    {.entry = uart::console_thread,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = false,
                     .cap_count = 2,
                     // SIGNAL is a pure post on the binding, not a raise at the controller.
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_SIGNAL}}}},
        .block_init = block_init
    };

    static_assert(drv::valid(k_desc), "the lx6uart descriptor is not a well-formed driver shape");
    static_assert(uart::desc_ok(k_desc), "the lx6uart cap positions do not match KOS_UART_CAP_*");
}

extern "C"
{

int lx6uart_console_start(struct kos_service_cfg const* cfg)
{
    return drv::bring_up(k_desc, cfg, nullptr);
}

}
