// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See <rxsci.h> for the interrupt sources and the ordering rules.

#include "rxsci.h"

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart_service.h>

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace drv = kickos::driver;
namespace uart = kickos::uart;
namespace mmap = kickos::rx::mmap;

namespace
{
    // Dedicated SCI6 vectors with their own INTB slot (arch/rx/chip/rx72m/startup.S).
    constexpr int SCI6_TXI_LINE = 87;
    constexpr int SCI6_RXI_LINE = 86;

    constexpr uart::UartParams k_uart = {
        .open_fail = "[rxsci] SCI6 open refused: source clock, divisor or frame",
        .announce = "[rxsci] device up (IRQ TX/RX)\n",
        // NO prime: TXI's only raise is a transfer taken with the source already armed, so a
        // pass that stopped with the ring loaded would wait on a transition that has already
        // happened. The announce leaves nothing loaded, and the call that takes a byte
        // disarms TIE.
        .prime = false
    };

    void irq_entry(void* arg)
    {
        uart::irq_thread<struct kos_uart>(static_cast<uart::Ctx*>(arg), k_uart);
    }

    int block_init(void* blk, struct kos_service_cfg const* cfg)
    {
        // hz travels as the REQUESTED baud; 0 keeps the divisor the kernel console left.
        return uart::ctx_init(static_cast<uart::Ctx*>(blk), cfg, /*fallback_baud=*/0u);
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[rxsci] ",
        .expected_base = mmap::SCI6,
        .block_size = uart::KOS_UART_BLOCK_SIZE,
        .ready_offset = uart::KOS_UART_READY_OFFSET,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 2,
        .thread_count = 3,
        .barrier_after = 2,
        // Both EDGE: a raise taken while the line is masked latches and redelivers on the
        // rearm, and leg L5 requires it of RXI, which is relayed by a thread holding no
        // window. TEI6 / ERI6 are LEVEL and are NOT claimed (see <rxsci.h>).
        .lines = {{SCI6_TXI_LINE, KOS_IRQ_EDGE}, {SCI6_RXI_LINE, KOS_IRQ_EDGE}},
        .threads = {{.entry = irq_entry,
                     .name = "rxsciirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = true,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT}}},
                    // barrier_after = 2 spawns this one BEFORE the readiness poll, which is
                    // sound only because it holds no WAIT cap on the endpoint: root stays the
                    // sole receiver, so a readiness timeout is still reportable.
                    //
                    // It takes no block argument and touches no ring, but it is a member of
                    // this driver's task and so its region set covers the whole block anyway.
                    // The fleet's only thread in that position; the DEV window, which is what
                    // isolation here is about, it does not have.
                    {.entry = drv::edge_relay_thread,
                     .name = "rxscirx",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 2,
                     .caps = {{drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_SIGNAL}}},
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

    static_assert(drv::valid(k_desc), "the rxsci descriptor is not a well-formed driver shape");
    static_assert(uart::desc_ok(k_desc), "the rxsci cap positions do not match KOS_UART_CAP_*");
}

extern "C"
{

int rxsci_console_start(struct kos_service_cfg const* cfg)
{
    return drv::bring_up(k_desc, cfg, nullptr);
}

}
