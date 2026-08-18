// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411/USART2 buffered IRQ-driven userspace UART driver (see f4uartirq.h).
//
// HARD RULE (design D7): NO libc stdio anywhere in this file. printf/puts route through
// _write -> kos_send(cap 0) -> this driver's own endpoint, and the service thread holds the
// only WAIT cap on it, so a self-send never returns.

#include "f4uartirq.h"

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/service.h>
#include <kickos/sys/uart_service.h>

#include <irq.h> // USART2_IRQ
#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace drv = kickos::driver;
namespace uart = kickos::uart;
namespace mmap = kickos::stm32f411::mmap;

namespace
{
    constexpr uart::UartParams k_uart = {
        .open_fail = "[f4uartirq] USART2 open refused: baud request or frame",
        .announce = "[f4uartirq] device up (IRQ TX/RX)\n",
        // prime=false is safe here: a LEVEL rearm discards the latch and then unmasks into a
        // line the peripheral still drives, so an RXNE that arrived during the announce
        // raises again at the first irq_wait rather than being lost. The announce's own
        // polled writes leave nothing armed on the TX side: kos_uart_write disarms TXEIE on
        // every call the device accepted whole.
        .prime = false
    };

    void irq_entry(void* arg)
    {
        uart::irq_thread<struct kos_uart>(static_cast<uart::Ctx*>(arg), k_uart);
    }

    int block_init(void* blk, struct kos_service_cfg const* cfg)
    {
        // A cfg naming no rate asks for 115200, not for the kernel's divisor: this driver
        // rewrites CR1/CR2/CR3 anyway, so BRR is written in the same pass.
        return uart::ctx_init(static_cast<uart::Ctx*>(blk), cfg, /*fallback_baud=*/115200u);
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[f4uartirq] ",
        // The USART2 vector below is claimed BY NUMBER, so a cfg naming USART1 or USART6
        // would grant one window and interrupt on another. Leg L9 refuses the descriptor
        // without this pin.
        .expected_base = mmap::USART2_BASE,
        .block_size = uart::KOS_UART_BLOCK_SIZE,
        .block_flags = 0,
        .ready_offset = uart::KOS_UART_READY_OFFSET,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 1,
        .thread_count = 2,
        .barrier_after = 1,
        // LEVEL: every USART event is ORed into one request line (RM0383 sec.19.4
        // Figure 191), so a status flag stays asserted until the driver clears it at the
        // peripheral. ORE is the one that matters: RXNEIE arms it, and a bare DR read does
        // not clear it.
        .lines = {{kickos::stm32f411::irq::USART2_IRQ, KOS_IRQ_LEVEL}},
        // Spawn ORDER is load-bearing: the IRQ thread first is what leaves the TX ring
        // provably empty when kos_uart_open puts the channel live.
        .threads = {{.entry = irq_entry,
                     .name = "uartirq",
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

    static_assert(drv::valid(k_desc),
                  "the f4uartirq descriptor is not a well-formed driver shape");
    static_assert(uart::desc_ok(k_desc),
                  "the f4uartirq cap positions do not match KOS_UART_CAP_*");
}

extern "C"
{

int f4uartirq_console_start(struct kos_service_cfg const* cfg)
{
    return drv::bring_up(k_desc, cfg, nullptr);
}

}
