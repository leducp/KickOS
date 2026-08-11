// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/UART0 buffered IRQ-driven userspace UART driver (see k64uartirq.h).
//
// HARD RULE (design D7): NO libc stdio anywhere in this file. printf/puts route through
// _write -> kos_send(cap 0) -> this driver's own endpoint, and the service thread holds the
// only WAIT cap on it, so a self-send never returns.

#include "k64uartirq.h"

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/service.h>
#include <kickos/sys/uart_service.h>

#include <irq.h> // UART0_RXTX_IRQ
#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace drv = kickos::driver;
namespace uart = kickos::uart;
namespace mmap = kickos::mk64f::mmap;

namespace
{
    constexpr uart::UartParams k_uart = {
        .open_fail = "[k64uartirq] UART0 open refused: AIPS enable, baud request or frame",
        .announce = "[k64uartirq] device up (IRQ TX/RX)\n",
        .prime = true
    };

    void irq_entry(void* arg)
    {
        uart::irq_thread<struct kos_uart>(static_cast<uart::Ctx*>(arg), k_uart);
    }

    int block_init(void* blk, struct kos_service_cfg const* cfg)
    {
        // A cfg naming no rate asks for 115200, not for the kernel's divisor: this driver
        // reprograms the frame anyway, so the divisor is written in the same pass.
        return uart::ctx_init(static_cast<uart::Ctx*>(blk), cfg, /*fallback_baud=*/115200u);
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[k64uartirq] ",
        // UART0_RXTX below is claimed BY NUMBER, so a cfg naming another instance would
        // grant one window and interrupt on another. Leg L9 refuses the descriptor without
        // this pin.
        .expected_base = mmap::UART0_BASE,
        .block_size = uart::KOS_UART_BLOCK_SIZE,
        .ready_offset = uart::KOS_UART_READY_OFFSET,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 1,
        .thread_count = 2,
        .barrier_after = 1,
        // LEVEL: every source on this vector is a status flag that stays asserted until
        // the driver clears it at the peripheral.
        .lines = {{kickos::mk64f::irq::UART0_RXTX_IRQ, KOS_IRQ_LEVEL}},
        .threads = {{.entry = irq_entry,
                     .name = "uartirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .mem_grant = true,
                     .window_grant = true,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT}}},
                    {.entry = uart::console_thread,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .mem_grant = true,
                     .window_grant = false,
                     .cap_count = 2,
                     // SIGNAL is a pure post on the binding, not a raise at the controller.
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_SIGNAL}}}},
        .block_init = block_init
    };

    static_assert(drv::valid(k_desc),
                  "the k64uartirq descriptor is not a well-formed driver shape");
    static_assert(uart::desc_ok(k_desc),
                  "the k64uartirq cap positions do not match KOS_UART_CAP_*");
}

extern "C"
{

int k64uartirq_console_start(struct kos_service_cfg const* cfg)
{
    return drv::bring_up(k_desc, cfg, nullptr);
}

}
