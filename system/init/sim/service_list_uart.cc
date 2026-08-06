// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host-sim service list carrying a LOOPBACK UART on the buffered-UART substrate: the
// two-thread driver, the shared SPSC rings and the TX doorbell, runnable with no board.
// Selected with -DKICKOS_SERVICE_LIST=kickos_services_simuart.
//
// The "device" is host fd 1 plus an internal loopback: every byte the driver transmits
// arrives back on RX. Sim-only by construction (host write(2)).
//
// This list deliberately publishes NO console. It is a KOS_SVC_UART port, so the kernel
// keeps its own console and both paths stay readable.
//
// There is no hardware TX-empty interrupt and no asynchronous RX here: the doorbell is
// the ONLY thing that wakes the IRQ thread. Nothing in this list exercises the
// transition-triggered half of RULE T1, because a host write cannot fail to raise.

#include <kickos/sys/service.h>

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/driver_bringup.h>
#include <kickos/sys/uart_service.h>

#include <stdint.h>

// The host write(2), declared rather than included: this TU is built freestanding and
// must not pull host headers. fd 1 is "the wire".
extern "C" long write(int, void const*, unsigned long);

namespace
{
    // Lines taken elsewhere: 30 (sim console ring), 28 (simcon window thread), 5..14 and
    // 20 (selftest arms). Nothing but the doorbell raises this one.
    constexpr int SIMUART_LINE = 29;

    // Bytes the modelled device accepts on a pass that finds it ready.
    constexpr uint32_t FIFO_DEPTH = 32;

    // Root's wait for the IRQ thread to reach its loop, in 1 ms steps.
    constexpr uint32_t READY_WAIT_MAX = 500u;
    constexpr uint64_t READY_WAIT_NS = 1000000u;

    // The shared ring block, handed to both threads as their thread ARG. It must come
    // from the user arena, naturally aligned, to satisfy the RAM arm of
    // grant_region_admissible for the grants below.
    kickos::uart::Shared* g_shared = nullptr;
    kos_cap_t g_uart_ep = KOS_CAP_NONE;

    // The per-chip class, sim edition. Every method here may be called ONLY from the IRQ
    // thread: on a real chip they touch the granted register window, which has exactly
    // one holder.
    struct LoopUart
    {
        kickos::uart::Shared* sh;

        uint32_t configure(uint32_t baud, uint8_t, uint8_t, uint8_t)
        {
            return baud; // no divisor to program: the host wire has no baud
        }

        bool tx_idle() const { return true; }

        void tx_irq_enable()
        {
            // Nothing to arm: host stdout is never busy, so there is no TX-empty source.
            // On silicon this is the CCR.TBIEN / SCR.TIE write, and RULE T1 lives here.
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
                    sh->stats.rx_dropped++;
                }
                else
                {
                    sh->stats.rx_bytes++;
                }
            }
        }
    };

    void uart_irq_thread(void* arg)
    {
        kickos::uart::Shared* sh = static_cast<kickos::uart::Shared*>(arg);
        LoopUart dev;
        dev.sh = sh;
        (void)dev.configure(115200u, 8u, KOS_UART_PARITY_NONE, 1u);
        kickos::uart::irq_loop(dev, sh); // parks in irq_wait; never returns
    }

    void uart_service_thread(void* arg)
    {
        kickos::uart::Shared* sh = static_cast<kickos::uart::Shared*>(arg);
        kickos::uart::serve_loop(sh); // parks in recv; returns when the endpoint dies
        kos_exit(0);
    }
}

extern "C"
{

// One-shot: the app takes the handle and delegates a SIGNAL-narrowed copy to each client
// it spawns.
kos_cap_t kickos_sim_uart_take_endpoint(void)
{
    kos_cap_t const ep = g_uart_ep;
    g_uart_ep = KOS_CAP_NONE;
    return ep;
}

static int sim_uart_start(struct kos_service_cfg const* cfg)
{
    if (cfg == nullptr or cfg->kind != KOS_SVC_UART)
    {
        kos::print("[simuart] ERROR: bad or non-UART service cfg\n");
        return -1;
    }

    // 1. The shared block: ONE power-of-two, naturally-aligned allocation, because the
    //    RAM arm of the grant predicate demands it of every caller including this one.
    void* blk = kos_ram_alloc(kickos::uart::KOS_UART_BLOCK_SIZE);
    if (blk == nullptr)
    {
        kos::print("[simuart] ERROR: arena cannot spare the ring block\n");
        return -1;
    }
    // Reach it before writing it. kos_ram_alloc hands back arena memory but grants
    // NOTHING: under enforcement root's own region set does not cover the arena, so
    // shared_init would fault on the block it just obtained.
    if (kos_mem_self_grant(blk, kickos::uart::KOS_UART_BLOCK_SIZE) != 0)
    {
        kos::print("[simuart] ERROR: mem_self_grant of the ring block refused\n");
        return -1;
    }
    g_shared = static_cast<kickos::uart::Shared*>(blk);
    kickos::uart::shared_init(g_shared);

    // 2. The request endpoint. Root KEEPS a full-rights cap so it can hand SIGNAL copies
    //    to clients; the driver's service thread gets WAIT only.
    kos_cap_t ep = KOS_CAP_NONE;
    if (kos_endpoint_create(&ep) != 0)
    {
        kos::print("[simuart] ERROR: endpoint_create failed\n");
        return -1;
    }

    // 3. The line. Claimed HERE because minting needs KOS_AUTH_IRQ and both driver
    //    threads run at authority 0. It comes back MASKED: the IRQ thread's first wait
    //    arms it, in the thread that will consume the event.
    kos_cap_t irq = KOS_CAP_NONE;
    if (kos_irq_claim(SIMUART_LINE, KOS_IRQ_EDGE, &irq) != 0)
    {
        kos::print("[simuart] ERROR: irq_claim failed\n");
        kos_handle_close(ep);
        return -1;
    }

    // 4. The IRQ thread: the line (WAIT) and the shared block. On silicon it would also
    //    take the register window; the SERVICE thread never can, because a DEV window has
    //    exactly one holder and the second spawn is refused -KOS_EBUSY. Its priority is
    //    strictly ABOVE the service thread: a device drain must preempt request serving.
    kos_cap_grant const irq_caps[1] = {{irq, KOS_CAP_WAIT}};
    auto const irqt = kos::thread::spawn(uart_irq_thread, g_shared, "uartirq",
                                         static_cast<uint8_t>(cfg->prio + 1),
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                         /*mem=*/g_shared,
                                         kickos::uart::KOS_UART_BLOCK_SIZE,
                                         /*stack=*/nullptr, /*stack_size=*/0,
                                         /*mmio=*/nullptr, 0, irq_caps, 1);
    if (not irqt.valid())
    {
        kos::print("[simuart] ERROR: IRQ thread spawn failed\n");
        kos_handle_close(irq);
        kos_handle_close(ep);
        return -1;
    }

    // 4b. MUST precede step 5, on two counts. No request may be served against a device
    //     that is not yet configured (uart_service.h, Shared::ready), and a timeout has
    //     to be reported while root is still the sole receiver: once the service thread
    //     holds a WAIT cap, recv_holders never reaches 0, nothing reclaims the console,
    //     and the diagnostic is dropped.
    {
        uint32_t waited = 0;
        while (g_shared->ready == 0u)
        {
            if (waited >= READY_WAIT_MAX)
            {
                kos_handle_close(irq);
                kos_handle_close(ep);
                kos::print("[simuart] ERROR: IRQ thread never reached its loop\n");
                return -1;
            }
            waited++;
            kos_sleep_ns(READY_WAIT_NS);
        }
    }

    // 5. The service thread: the endpoint (WAIT) at index 1 and the SAME line as the
    //    DOORBELL (SIGNAL only) at index 2. SIGNAL is not "raise the line at the
    //    controller"; it is a pure post on the binding, which is what lets this thread
    //    start a transfer without touching a register it does not own.
    kos_cap_grant const svc_caps[2] = {{ep, KOS_CAP_WAIT}, {irq, KOS_CAP_SIGNAL}};
    auto const svct = kos::thread::spawn(uart_service_thread, g_shared, cfg->name,
                                         cfg->prio, KOS_POLICY_FIFO, 0,
                                         /*privileged=*/false,
                                         /*mem=*/g_shared,
                                         kickos::uart::KOS_UART_BLOCK_SIZE,
                                         /*stack=*/nullptr, /*stack_size=*/0,
                                         /*mmio=*/nullptr, 0, svc_caps, 2);
    if (not svct.valid())
    {
        kos::print("[simuart] ERROR: service thread spawn failed\n");
        kos_handle_close(irq);
        kos_handle_close(ep);
        return -1;
    }

    // Root's own line cap goes, leaving the two driver threads as the only holders, so
    // the line returns to the pool when BOTH die.
    kos_handle_close(irq);
    g_uart_ep = ep;
    kos::print("[simuart] UART service up (loopback over fd 1, IRQ-paced by doorbell)\n");
    return 0;
}

static struct kos_service_cfg const simuart_cfg = {
    .name = "simuart",
    .mmio_base = 0,
    .mmio_window = 0,
    .hz = 0,
    .addr = 0,
    .prio = 12,
    .kind = KOS_SVC_UART,
    .rsv = { 0, 0, 0, 0 }
};

static struct kos_service_bringup const simuart_services[] = {
    {sim_uart_start, &simuart_cfg},
};
struct kos_service_list const kickos_board_services = {simuart_services, 1};
}
