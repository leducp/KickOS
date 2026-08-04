// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32 (Xtensa LX6) UART0 buffered userspace UART driver on the
// <kickos/sys/uart_service.h> substrate: a privileged one-shot bring-up plus two
// unprivileged threads, one parked in kos_irq_wait owning every UART0 register, one
// parked in kos_recv owning none of them.
//
// The kernel console line cannot be claimed while the kernel's own TX ring holds it, so
// the bring-up publishes the console to this service's endpoint FIRST: that disarms the
// ring, detaches the line and routes stdout here. A plain send is therefore a raw console
// write and a kos_call is a <kickos/sys/uart.h> request; the service loop demuxes on
// which one arrived.
//
// Register facts come from the ESP32 TRM v5.8 ch.19 via arch/xtensa/chip/esp32/regs/uart.h.
// Three of them drive the whole IRQ discipline:
//
//   - UART_INT_ST == UART_INT_RAW & UART_INT_ENA, and RAW is a LATCH that only an
//     INT_CLR write drops (TRM appendix "Interrupt Configuration Registers"). So the
//     status read needs no software masking, and every serviced source needs a clear.
//   - UART_TXFIFO_EMPTY's condition is level on occupancy: "when the data amount in
//     transmit-FIFO is less than its threshold value, it will produce a
//     TXFIFO_EMPTY_INT_RAW interrupt" (TRM Register 19.10). Enabling it on an idle
//     channel therefore raises immediately, so RULE T1's prime is not load-bearing here.
//   - UART_RXFIFO_FULL_INT_CLR "can be set only when data in Rx_FIFO is less than
//     UART_RXFIFO_FULL_THRHD" (TRM Register 19.5). With the threshold at 1 the clear
//     takes ONLY on a fully drained FIFO, so it must follow the drain; a byte landing
//     mid-pass legitimately refuses it, which re-posts the line rather than losing it.
//
// UART_RXFIFO_TOUT is deliberately never enabled: its clear is gated on rxfifo_cnt AND
// rx_mem_cnt both being 0, a condition this driver cannot guarantee at clear time, and
// with RXFIFO_FULL_THRHD at 1 there is nothing left for it to pace.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/io/mmio.h>
#include <kickos/sys/driver_bringup.h> // console_handover_finish
#include <kickos/sys/service.h>
#include <kickos/sys/uart_service.h>

#include "irq.h"
#include "mmap.h"
#include "regs/uart.h"

#include <stdint.h>

namespace reg = kickos::esp32::reg;
namespace mmap = kickos::esp32::mmap;
namespace lx6irq = kickos::esp32::irq;

namespace
{
    // RX sources the driver arms. TX-empty is armed on demand (service_irq), never here:
    // its condition holds on an empty FIFO, so arming it with an empty ring is a
    // self-sustaining storm.
    constexpr uint32_t RX_INT_MASK = reg::uart::RXFIFO_FULL_INT | reg::uart::RXFIFO_OVF_INT
                                     | reg::uart::FRM_ERR_INT | reg::uart::PARITY_ERR_INT;

    // Per-byte cap on the first-light poll, so a FIFO that never drains costs a delay
    // rather than the IRQ thread.
    constexpr uint32_t TX_SPIN_MAX = 1000000u;

    // Bound on the wait for the IRQ thread's own bring-up. Sleeping, not spinning: that
    // thread may sit below root's priority, so a spin would never let it run.
    constexpr uint32_t READY_WAIT_NS = 1000000u; // 1 ms
    constexpr uint32_t READY_WAIT_MAX = 1000;    // ~1 s total

    kickos::uart::Shared* g_shared = nullptr;

    // Every method touches the granted register window: IRQ thread only.
    struct Lx6Uart
    {
        kickos::uart::Shared* sh;
        uintptr_t win;

        uint32_t txfifo_cnt() const
        {
            return (r32(win + reg::uart::OFF_STATUS) >> reg::uart::TXFIFO_CNT_SHIFT)
                   & reg::uart::TXFIFO_CNT_MASK;
        }

        uint32_t rxfifo_cnt() const
        {
            return (r32(win + reg::uart::OFF_STATUS) >> reg::uart::RXFIFO_CNT_SHIFT)
                   & reg::uart::RXFIFO_CNT_MASK;
        }

        bool tx_idle() const { return txfifo_cnt() == 0u; }

        void tx_irq_enable()
        {
            // Enable only. The stale latch is dropped per push instead: dropping it here
            // would kill the re-raise when the loop stopped on a full FIFO.
            r32(win + reg::uart::OFF_INT_ENA) =
                r32(win + reg::uart::OFF_INT_ENA) | reg::uart::TXFIFO_EMPTY_INT;
        }

        void tx_irq_disable()
        {
            r32(win + reg::uart::OFF_INT_ENA) =
                r32(win + reg::uart::OFF_INT_ENA) & ~reg::uart::TXFIFO_EMPTY_INT;
        }

        // CONF0 is deliberately not rewritten. The ROM left UART0 at Register 19.9's reset
        // framing, which is the 8N1 being asked for, and reprogramming framing or CLKDIV
        // during the console handover would reframe a byte still in flight. The rate the
        // channel actually runs at is returned, not the one requested.
        uint32_t configure(uint32_t, uint8_t, uint8_t, uint8_t)
        {
            r32(win + reg::uart::OFF_INT_ENA) = 0;
            r32(win + reg::uart::OFF_INT_CLR) = 0xFFFFFFFFu;
            r32(win + reg::uart::OFF_CONF1) =
                ((reg::uart::TXFIFO_EMPTY_THRHD & reg::uart::TXFIFO_EMPTY_THRHD_MASK)
                 << reg::uart::TXFIFO_EMPTY_THRHD_SHIFT)
                | ((reg::uart::RXFIFO_FULL_THRHD & reg::uart::RXFIFO_FULL_THRHD_MASK)
                   << reg::uart::RXFIFO_FULL_THRHD_SHIFT);
            r32(win + reg::uart::OFF_INT_ENA) = RX_INT_MASK;
            return reg::uart::CONSOLE_BAUD;
        }

        // Direct-to-FIFO diagnostic, not stdio and not the ring. Runs with
        // TXFIFO_EMPTY_INT still disabled, so it raises nothing.
        void win_puts(char const* s)
        {
            for (; *s != '\0'; s++)
            {
                for (uint32_t i = 0; i < TX_SPIN_MAX; i++)
                {
                    if (txfifo_cnt() < reg::uart::TXFIFO_LIMIT)
                    {
                        r32(win + reg::uart::OFF_FIFO) =
                            static_cast<uint32_t>(static_cast<unsigned char>(*s));
                        break;
                    }
                }
            }
        }

        // A wake is not proof of a hardware event (the doorbell is a pure post), so
        // finding nothing asserted must be harmless.
        void service_irq()
        {
            uint32_t const st = r32(win + reg::uart::OFF_INT_ST);

            // UART_FIFO_REG carries the data byte only, with no per-byte error tag, so an
            // error cannot be attributed to one byte here: it is counted and dropped, and
            // the offending byte stays in the stream. These three latches are ungated, so
            // unlike the RX-full clear below they take immediately.
            uint32_t err_clr = 0;
            if ((st & reg::uart::RXFIFO_OVF_INT) != 0)
            {
                sh->stats.rx_overrun++;
                err_clr |= reg::uart::RXFIFO_OVF_INT;
            }
            if ((st & reg::uart::FRM_ERR_INT) != 0)
            {
                sh->stats.rx_framing++;
                err_clr |= reg::uart::FRM_ERR_INT;
            }
            if ((st & reg::uart::PARITY_ERR_INT) != 0)
            {
                sh->stats.rx_parity++;
                err_clr |= reg::uart::PARITY_ERR_INT;
            }
            if (err_clr != 0)
            {
                r32(win + reg::uart::OFF_INT_CLR) = err_clr;
            }

            drain_rx();
            fill_tx();
        }

        // Read what the FIFO holds NOW, one bounded pass. A byte arriving mid-pass leaves
        // the threshold condition true, so the clear below is refused and the line
        // re-posts; nothing is lost by not looping here.
        void drain_rx()
        {
            uint32_t const cnt = rxfifo_cnt();
            for (uint32_t i = 0; i < cnt; i++)
            {
                unsigned char b =
                    static_cast<unsigned char>(r32(win + reg::uart::OFF_FIFO) & 0xFFu);
                // A full RX ring drops the NEWEST byte and counts it. Refusing to read the
                // FIFO instead would turn a counted software overflow into a hardware
                // overrun plus a permanently asserted source, i.e. a storm, not a loss.
                if (kos_byte_ring_push(&sh->rx, &b, 1) == 0u)
                {
                    sh->stats.rx_dropped++;
                }
                else
                {
                    sh->stats.rx_bytes++;
                }
            }
            // AFTER the drain, and only then does the hardware accept it (Register 19.5).
            r32(win + reg::uart::OFF_INT_CLR) = reg::uart::RXFIFO_FULL_INT;
        }

        void fill_tx()
        {
            unsigned char b = 0;
            while (txfifo_cnt() < reg::uart::TXFIFO_LIMIT
                   and kos_byte_ring_pop_one(&sh->tx, &b) == 1)
            {
                r32(win + reg::uart::OFF_FIFO) = b;
                // The latch survives the FIFO passing the threshold, so it is dropped per
                // push rather than once at the end of the burst.
                r32(win + reg::uart::OFF_INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;
            }
            if (kos_byte_ring_used(&sh->tx) == 0u)
            {
                tx_irq_disable();
            }
            else
            {
                tx_irq_enable(); // the FIFO filled: come back when it drains past the threshold
            }
        }
    };

    void lx6uart_irq_thread(void* arg)
    {
        kickos::uart::Shared* sh = static_cast<kickos::uart::Shared*>(arg);
        Lx6Uart dev;
        dev.sh = sh;
        // No kos_periph_enable and no DPORT clock ungate: the ROM ran its boot log through
        // UART0, so the block is already clocked out of reset.
        dev.win = mmap::UART0_BASE;
        (void)dev.configure(reg::uart::CONSOLE_BAUD, 8u, KOS_UART_PARITY_NONE, 1u);
        dev.win_puts("[lx6uart] device up (IRQ TX/RX)\n");
        kickos::uart::irq_loop(dev, sh); // parks in irq_wait; never returns
    }

    // Not kickos::uart::serve_loop: this endpoint carries TWO protocols. A kos_call is a
    // kos_uart_req frame, a plain send is raw console bytes, and serve_one refuses a plain
    // send, which for a console would silently swallow every print.
    void lx6uart_service_thread(void* arg)
    {
        kickos::uart::Shared* sh = static_cast<kickos::uart::Shared*>(arg);
        unsigned char msg[KOS_EP_MSG_MAX];
        while (true)
        {
            struct kos_recv_info info;
            long const n = kos_recv(kickos::uart::KOS_UART_CAP_EP, msg, sizeof(msg), &info);
            if (n < 0)
            {
                break; // endpoint dead: let the bring-up respawn us
            }
            if (info.reply_cap != KOS_CAP_NONE)
            {
                kickos::uart::serve_one(sh, msg, static_cast<size_t>(n), info.reply_cap);
                continue;
            }
            if (n == 0)
            {
                (void)kickos::uart::console_flush(sh); // zero-length plain send == flush
                continue;
            }
            // Not a bare tx_write: a plain send cannot report a short accept, so a producer
            // that outruns the 115200 baud wire would have its tail silently spliced away.
            // Staying out of kos_recv until the ring took it all is what paces it.
            uint32_t const took =
                kickos::uart::console_write_all(sh, msg, static_cast<uint32_t>(n));
            sh->stats.tx_dropped += static_cast<uint32_t>(n) - took;
        }
        kos_exit(0);
    }
}

extern "C"
{

int lx6uart_console_start(struct kos_service_cfg const* cfg)
{
    if (cfg == nullptr or cfg->kind != KOS_SVC_CONSOLE)
    {
        kos::print("[lx6uart] ERROR: bad or non-console service cfg\n");
        return -1;
    }
    // The class is hard-wired to UART0 (the console), so a cfg naming another window would
    // grant one block and poke another.
    if (cfg->mmio_base != mmap::UART0_BASE)
    {
        kos::print("[lx6uart] ERROR: cfg mmio_base is not UART0\n");
        return -1;
    }

    // 1. The shared block: ONE power-of-two, naturally-aligned allocation, because the RAM
    //    arm of the grant predicate demands it of every caller including this one.
    void* blk = kos_ram_alloc(kickos::uart::KOS_UART_BLOCK_SIZE);
    if (blk == nullptr)
    {
        kos::print("[lx6uart] ERROR: arena cannot spare the ring block\n");
        return -1;
    }
    // Reach it before writing it: kos_ram_alloc hands back arena memory but grants nothing.
    if (kos_mem_self_grant(blk, kickos::uart::KOS_UART_BLOCK_SIZE) != 0)
    {
        kos::print("[lx6uart] ERROR: mem_self_grant of the ring block refused\n");
        return -1;
    }
    g_shared = static_cast<kickos::uart::Shared*>(blk);
    kickos::uart::shared_init(g_shared);

    kos_cap_t ep = KOS_CAP_NONE;
    if (kos_endpoint_create(&ep) != 0)
    {
        kos::print("[lx6uart] ERROR: endpoint_create failed\n");
        return -1;
    }

    // 2. Relinquish the kernel UART. This is also what frees the grouped UART0 line: the
    //    kernel's TX ring holds it via irq_attach and irq_claim below would be refused
    //    while it does. The detach masks CPU interrupt 13 in INTENABLE, so on return the
    //    console path is dark, drained, and raising nothing.
    if (kos_console_publish(ep) != 0)
    {
        kos::print("[lx6uart] ERROR: console_publish failed\n");
        kos_handle_close(ep);
        return -1;
    }

    // 3. The line, claimed HERE because minting needs KOS_AUTH_IRQ and both driver threads
    //    run at authority 0. LEVEL: CPU interrupt 13 is level-triggered (TRM Table 8.3-2)
    //    and the UART latch stays set until the driver clears it, so the rearm must discard
    //    a stale pending first. It comes back MASKED: the IRQ thread's first wait arms it,
    //    in the thread that will consume the event.
    kos_cap_t irq = KOS_CAP_NONE;
    if (kos_irq_claim(lx6irq::CONSOLE_TX_LINE, KOS_IRQ_LEVEL, &irq) != 0)
    {
        kos_handle_close(ep); // closing reclaims the console, so the tag reaches the wire
        kos::print("[lx6uart] ERROR: irq_claim failed\n");
        return -1;
    }

    // 4. The IRQ thread: the register window (R|W|DEV), the line (WAIT) and the shared
    //    block. Strictly ABOVE the service thread.
    kos_cap_grant const irq_caps[1] = {{irq, KOS_CAP_WAIT}};
    auto const irqt = kos::thread::spawn(lx6uart_irq_thread, g_shared, "lx6uartirq",
                                         static_cast<uint8_t>(cfg->prio + 1),
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                         /*mem=*/g_shared,
                                         kickos::uart::KOS_UART_BLOCK_SIZE,
                                         /*stack=*/nullptr, /*stack_size=*/0,
                                         /*mmio=*/reinterpret_cast<void*>(cfg->mmio_base),
                                         cfg->mmio_window, irq_caps, 1);
    if (not irqt.valid())
    {
        kos_handle_close(irq);
        kos_handle_close(ep); // closing reclaims the console, so the tag reaches the wire
        kos::print("[lx6uart] ERROR: IRQ thread spawn failed\n");
        return -1;
    }

    // 4b. Wait for the IRQ thread's own bring-up BEFORE the service thread exists. The
    //     handover probe further down proves only that the SERVICE thread is serving, and
    //     would succeed against a device that was never configured. The ordering is what
    //     makes a timeout REPORTABLE: root is still the sole receiver here, so closing the
    //     endpoint reclaims the console and the diagnostic reaches the wire.
    uint32_t waited = 0;
    while (g_shared->ready == 0u)
    {
        if (waited >= READY_WAIT_MAX)
        {
            kos_handle_close(irq);
            kos_handle_close(ep);
            kos::print("[lx6uart] ERROR: IRQ thread never reached its loop\n");
            return -1;
        }
        waited++;
        kos_sleep_ns(READY_WAIT_NS);
    }

    // 5. The service thread: the endpoint (WAIT) and the SAME line as the DOORBELL (SIGNAL
    //    only). SIGNAL is a pure post on the binding, not a raise at the controller: on
    //    this chip INTSET cannot raise a real line at all. No MMIO window, because a DEV
    //    window has exactly one holder.
    kos_cap_grant const svc_caps[2] = {{ep, KOS_CAP_WAIT}, {irq, KOS_CAP_SIGNAL}};
    auto const svct = kos::thread::spawn(lx6uart_service_thread, g_shared, cfg->name,
                                         cfg->prio, KOS_POLICY_FIFO, 0,
                                         /*privileged=*/false,
                                         /*mem=*/g_shared,
                                         kickos::uart::KOS_UART_BLOCK_SIZE,
                                         /*stack=*/nullptr, /*stack_size=*/0,
                                         /*mmio=*/nullptr, 0, svc_caps, 2);
    if (not svct.valid())
    {
        kos_handle_close(irq);
        kos_handle_close(ep);
        kos::print("[lx6uart] ERROR: service thread spawn failed\n");
        return -1;
    }

    // Root's own line cap goes: with the two driver threads the only holders, the line
    // returns to the pool when BOTH die.
    kos_handle_close(irq);

    return kickos::driver::console_handover_finish(
        ep, "[lx6uart] ERROR: driver died during bring-up\n");
}

}
