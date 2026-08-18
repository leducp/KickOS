// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 IRQ-driven buffered UART console driver on USIC0 CH0 (see
// <kickos/driver/xmcuartirq.h>).
//
// The USIC behaviour cited below is clean-room from the XMC4700/XMC4800 Reference Manual
// (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. "RM p.NN" are the manual's printed
// pages.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/xmcuartirq.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/service.h>
#include <kickos/sys/uart_service.h>

#include <irq.h> // kickos::xmc::irq::USIC0_SR0
#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace drv = kickos::driver;
namespace uart = kickos::uart;
namespace mmap = kickos::xmc::mmap;

namespace
{
    constexpr uart::UartParams k_uart = {
        .open_fail = "[xmcuartirq] U0C0 open refused: CCR write, baud request or frame",
        .announce = "[xmcuartirq] device up (IRQ TX)\n",
        // EDGE and still primed: kos_uart_open has already set TBIEN, so the announce's
        // polled writes raise transmit-buffer events that the first irq_wait discards.
        .prime = true
    };

    void irq_entry(void* arg)
    {
        uart::irq_thread<struct kos_uart>(static_cast<uart::Ctx*>(arg), k_uart);
    }

    int block_init(void* blk, struct kos_service_cfg const* cfg)
    {
        // hz travels as the REQUESTED baud; 0 keeps the divisor the kernel left. The frame
        // is what kickos_xmc_usic_init programmed, so the backend has nothing to reprogram.
        return uart::ctx_init(static_cast<uart::Ctx*>(blk), cfg, /*fallback_baud=*/0u);
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[xmcuartirq] ",
        // SR0 below is claimed BY NUMBER, so a cfg naming the sibling channel would grant
        // one window and interrupt on the other. The console owns U0C0; U0C1 is the SPI
        // bus. Leg L9 refuses the descriptor without this pin.
        .expected_base = mmap::USIC0_CH0_BASE,
        .block_size = uart::KOS_UART_BLOCK_SIZE,
        .block_flags = 0,
        .ready_offset = uart::KOS_UART_READY_OFFSET,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 1,
        .thread_count = 2,
        .barrier_after = 1,
        // EDGE, with no peripheral-side clear to pair with it: PSR.TBIF has no influence on
        // interrupt generation and does not need clearing (RM 18.2.2.3 p.18-17).
        .lines = {{kickos::xmc::irq::USIC0_SR0, KOS_IRQ_EDGE}},
        // Spawn ORDER is load-bearing: the IRQ thread first is what leaves the TX ring
        // provably empty when kos_uart_open arms TBIEN. The line's FIRST irq_wait DISCARDS
        // any pend latched before it, so a byte pushed earlier would lose its
        // transmit-buffer event and stall until the next doorbell.
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
                  "the xmcuartirq descriptor is not a well-formed driver shape");
    static_assert(uart::desc_ok(k_desc),
                  "the xmcuartirq cap positions do not match KOS_UART_CAP_*");
}

extern "C"
{

int xmcuartirq_console_start(struct kos_service_cfg const* cfg)
{
    return drv::bring_up(k_desc, cfg, nullptr);
}

}
