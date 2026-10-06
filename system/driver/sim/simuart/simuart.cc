// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The host sim's LOOPBACK UART on the buffered-UART substrate: the two-thread driver, the shared
// SPSC rings and the TX doorbell, runnable with no board.
//
// The "device" is host fd 1 plus an internal loopback: every byte the driver transmits
// arrives back on RX. Sim-only by construction (host write(2)).
//
// There is no hardware TX-empty interrupt and no asynchronous RX here: the doorbell is
// the ONLY thing that wakes the IRQ thread. Nothing here exercises a TX-empty source that
// raises only on a transition, because a host write cannot fail to raise.


#include <kickos/driver/declared/simuart.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart_service.h>

#include <stddef.h> // offsetof
#include <stdint.h>

// The host write(2), declared rather than included: this TU is built freestanding and
// must not pull host headers. fd 1 is "the wire".
extern "C" long write(int, void const*, unsigned long);

namespace drv = kickos::driver;
namespace uart = kickos::uart;
namespace declared = kickos::driver::declared::simuart;

namespace
{
    // Bytes the modelled device accepts on a pass that finds it ready.
    constexpr uint32_t FIFO_DEPTH = 32;

    // The per-chip class, sim edition. Every method here may be entered ONLY from the IRQ
    // thread: on a real chip they touch the granted register window, which has exactly
    // one holder.
    struct LoopUart
    {
        uart::Shared* sh;

        uint32_t configure(uint32_t baud, uint8_t, uint8_t, uint8_t)
        {
            return baud; // no divisor to program: the host wire has no baud
        }

        bool tx_idle() const { return true; }

        void tx_irq_enable()
        {
            // Nothing to arm: host stdout is never busy, so there is no TX-empty source. On
            // silicon the CCR.TBIEN / SCR.TIE write and the burst's first byte both go here.
        }

        // Wakes since boot. The readiness model below is a function of this alone, so it
        // stays deterministic on any host: no wall clock, no host scheduling.
        uint32_t pass = 0;

        // A wake is not proof of an event (the doorbell is a pure post), so finding an
        // empty ring must be harmless.
        //
        // Two properties of a real UART are modelled deliberately; without them a host
        // write(2) never blocks, the ring never fills, and back-pressure is never
        // exercised. FIFO_DEPTH bounds the bytes one pass can move, so a pass can end
        // with the ring still non-empty. The alternating not-ready pass stands in for a
        // byte taking ~87 us at 115200 baud against a pass of microseconds: on silicon
        // most wakes find no room and move NOTHING.
        void service_irq()
        {
            pass++;
            if ((pass & 1u) == 0u)
            {
                return; // device busy: this wake moves nothing
            }
            unsigned char b = 0;
            uint32_t budget = FIFO_DEPTH;
            while (budget-- > 0 and kos_byte_ring_pop_one(&sh->tx, &b) == 1)
            {
                (void)write(1, &b, 1);
                // LOOPBACK: the transmitted byte comes back as received. A full RX ring
                // drops the NEWEST byte and counts it. Refusing to read the device
                // instead would turn a counted software overflow into a hardware overrun
                // plus a stuck level interrupt: a storm rather than a loss.
                if (kos_byte_ring_push(&sh->rx, &b, 1) == 0u)
                {
                    kos_counter_increment(&sh->stats.rx_dropped, 1u);
                }
                else
                {
                    kos_counter_increment(&sh->stats.rx_bytes, 1u);
                }
            }
        }
    };

    void uart_irq_thread(void* arg)
    {
        uart::Shared* sh = static_cast<uart::Shared*>(arg);
        LoopUart dev;
        dev.sh = sh;
        (void)dev.configure(115200u, 8u, KOS_UART_PARITY_NONE, 1u);
        uart::irq_loop(dev, sh); // parks in irq_wait; never returns
    }

    void uart_service_thread(void* arg)
    {
        uart::Shared* sh = static_cast<uart::Shared*>(arg);
        uart::serve_loop(sh); // parks in recv; returns when the endpoint dies
        kos_exit(0);
    }

    int block_init(void* blk, struct kos_driver_instance const*)
    {
        uart::shared_init(static_cast<uart::Shared*>(blk));
        return 0;
    }

    // The block is a bare Shared, not a Ctx: this backend opens no kos_uart, so there is no
    // class config to carry. KOS_UART_READY_OFFSET still locates the latch only because
    // Ctx's first member IS the Shared, which is what this pins.
    static_assert(uart::KOS_UART_READY_OFFSET == offsetof(uart::Shared, ready),
                  "the sim block is a bare Shared, so the latch must sit at the Shared's "
                  "own offset");

    constexpr drv::Descriptor k_desc = {
        .tag = "[simuart] ",
        // No guard: no thread takes a window.
        .expected_base = 0,
        .block_size = declared::k_declared.block_size,
        .block_flags = 0,
        .ready_offset = uart::KOS_UART_READY_OFFSET,
        .ep_posture = declared::k_declared.ep_posture, // no kos_console_publish, no handover tail
        .line_count = declared::k_declared.line_count,
        .thread_count = declared::k_declared.thread_count,
        .barrier_after = declared::k_declared.barrier_after,
        // EDGE, and leg L5 requires it: the IRQ thread holds no window here, so it could not
        // clear a peripheral flag if there were one.
        .lines = {{KOS_IRQ_EDGE}},
        .threads = {{.entry = uart_irq_thread,
                     .name = declared::k_declared.thread_name[0],
                     .prio_delta = declared::k_declared.prio_delta[0],
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = false,
                     .cap_count = 2,
                     .caps = {{drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0}}},
                    {.entry = uart_service_thread,
                     .name = declared::k_declared.thread_name[1],
                     .prio_delta = declared::k_declared.prio_delta[1],
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = false,
                     .cap_count = 2,
                     // A BADGED copy of the same notification: a pure raise of the
                     // doorbell's own bit, never a touch of the controller.
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_NOTIFY, KOS_CAP_SIGNAL,
                               drv::doorbell_badge(1)}}}},
        .block_init = block_init
    };

    static_assert(drv::valid(k_desc), "the simuart descriptor is not a well-formed driver shape");
    static_assert(uart::desc_ok(k_desc), "the simuart cap positions do not match KOS_UART_CAP_*");
    static_assert(drv::declared_as(k_desc, declared::k_declared),
                  "the simuart descriptor departs from its kickos_add_driver declaration");
}

extern "C"
{

int simuart_start(struct kos_driver_instance* instance)
{
    int const rc = drv::bring_up(k_desc, instance);
    if (rc != 0)
    {
        return rc;
    }
    kos::print("[simuart] UART service up (loopback over fd 1, IRQ-paced by doorbell)\n");
    return 0;
}

}
