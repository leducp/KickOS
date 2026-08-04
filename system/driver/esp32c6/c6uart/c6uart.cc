// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6/UART0 buffered userspace UART driver on the <kickos/sys/uart_service.h>
// substrate: a privileged one-shot bring-up plus two unprivileged threads, one parked in
// kos_irq_wait owning every UART0 register, one parked in kos_recv owning neither.
//
// The kernel console line (the grouped UART0 line) cannot be claimed while the kernel's own
// TX ring holds it, so the bring-up publishes the console to this service's endpoint FIRST:
// that disarms the ring, detaches the line and routes stdout here. A plain send is therefore
// a raw console write and a kos_call is a <kickos/sys/uart.h> request; the service loop
// below demuxes on which one arrived.
//
// Register offsets and bit fields come from the ESP32-C6 TRM v1.2 ch.27 via
// arch/riscv/chip/esp32c6/regs/uart.h. Two chip facts drive the whole IRQ discipline:
//
//   - UART_INT_RAW is a LATCH (R/WTC/SS, TRM Register 27.3 and the Access Types glossary):
//     a bit is self-set by its condition and dropped only by writing 1 to the matching
//     UART_INT_CLR bit. It does NOT follow the condition back down.
//   - UART_TXFIFO_EMPTY's condition is LEVEL on occupancy, the TX FIFO holding less than
//     UART_TXFIFO_EMPTY_THRHD (TRM section 27.4.11), so enabling it on an idle channel
//     raises immediately and RULE T1's prime is unnecessary on this chip.
//
// Together they mean the latch must be dropped after every FIFO push, and must NOT be
// dropped when the loop stops because the FIFO filled: clearing it there with occupancy
// above the threshold would leave nothing to re-raise and the burst would stall.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/io/mmio.h>
#include <kickos/sys/driver_bringup.h> // console_handover_finish
#include <kickos/sys/service.h>
#include <kickos/sys/uart_service.h>

#include "irq.h"
#include "regs/uart.h"

#include <stdint.h>

namespace reg = kickos::esp32c6::reg;
namespace mmap = kickos::esp32c6::mmap;
namespace c6irq = kickos::esp32c6::irq;

namespace
{
    // Threshold 1: every byte raises. The alternative pacing source, UART_RXFIFO_TOUT,
    // needs UART_TOUT_CONF_SYNC plus a UART_REG_UPDATE synchronisation this driver does
    // not do.
    constexpr uint32_t RX_FULL_THRHD = 1;
    constexpr uint32_t TX_EMPTY_THRHD = 32;

    // RX sources the driver arms. TX-empty is armed on demand (service_irq), never here:
    // it is level on occupancy, so arming it with an empty ring is a self-sustaining storm.
    constexpr uint32_t RX_INT_MASK = reg::uart::RXFIFO_FULL_INT | reg::uart::RXFIFO_OVF_INT
                                     | reg::uart::FRM_ERR_INT | reg::uart::PARITY_ERR_INT;

    // Per-byte cap on the first-light poll, so a FIFO that never reports a free slot costs
    // a delay rather than the IRQ thread.
    constexpr uint32_t TX_SPIN_MAX = 1000000u;

    // Bound on the wait for the IRQ thread's own bring-up. Sleeping, not spinning: that
    // thread may sit below root's priority, so a spin would never let it run.
    constexpr uint32_t READY_WAIT_NS = 1000000u; // 1 ms
    constexpr uint32_t READY_WAIT_MAX = 1000;    // ~1 s total

    kickos::uart::Shared* g_shared = nullptr;

    // Every method touches the granted register window: IRQ thread only.
    struct C6Uart
    {
        kickos::uart::Shared* sh;
        uintptr_t win;

        uint32_t tx_slot_free() const
        {
            uint32_t const cnt = (r32(win + reg::uart::OFF_STATUS) >> reg::uart::TXFIFO_CNT_S)
                                 & reg::uart::TXFIFO_CNT_MASK;
            if (cnt < reg::uart::TXFIFO_LIMIT)
            {
                return 1u;
            }
            return 0u;
        }

        bool tx_idle() const
        {
            uint32_t const cnt = (r32(win + reg::uart::OFF_STATUS) >> reg::uart::TXFIFO_CNT_S)
                                 & reg::uart::TXFIFO_CNT_MASK;
            return cnt == 0u;
        }

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

        // The baud divisor is NOT reprogrammed: UART_CLKDIV_SYNC needs a UART_REG_UPDATE
        // synchronisation against a core-clock source this driver does not own, and the ROM
        // already brought UART0 up for its boot log. `baud` is echoed back rather than
        // measured, so the return value is NOT the achieved rate: the wire runs at whatever
        // the ROM left, and a mismatched request is not detectable here.
        uint32_t configure(uint32_t baud, uint8_t, uint8_t, uint8_t)
        {
            r32(win + reg::uart::OFF_INT_ENA) = 0;
            r32(win + reg::uart::OFF_INT_CLR) = 0xFFFFFFFFu;
            uint32_t conf1 = r32(win + reg::uart::OFF_CONF1);
            conf1 &= ~(reg::uart::RXFIFO_FULL_THRHD_MASK << reg::uart::RXFIFO_FULL_THRHD_S);
            conf1 &= ~(reg::uart::TXFIFO_EMPTY_THRHD_MASK << reg::uart::TXFIFO_EMPTY_THRHD_S);
            conf1 |= (RX_FULL_THRHD & reg::uart::RXFIFO_FULL_THRHD_MASK)
                     << reg::uart::RXFIFO_FULL_THRHD_S;
            conf1 |= (TX_EMPTY_THRHD & reg::uart::TXFIFO_EMPTY_THRHD_MASK)
                     << reg::uart::TXFIFO_EMPTY_THRHD_S;
            r32(win + reg::uart::OFF_CONF1) = conf1;
            r32(win + reg::uart::OFF_INT_ENA) = RX_INT_MASK;
            return baud;
        }

        // Direct-to-FIFO diagnostic, not stdio and not the ring. Runs with
        // TXFIFO_EMPTY_INT still disabled, so it raises nothing.
        void win_puts(char const* s)
        {
            for (; *s != '\0'; s++)
            {
                for (uint32_t i = 0; i < TX_SPIN_MAX; i++)
                {
                    if (tx_slot_free() != 0u)
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
            // error flag cannot be attributed to one byte here: it is counted and dropped,
            // and a stored erroneous byte stays in the stream. UART_ERR_WR_MASK would make
            // the hardware discard it, but it lives in UART_CONF0_SYNC.
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
        // the threshold condition true, so the latch re-sets after the clear below and this
        // runs again; nothing is lost by not looping here.
        void drain_rx()
        {
            uint32_t const cnt = (r32(win + reg::uart::OFF_STATUS) >> reg::uart::RXFIFO_CNT_S)
                                 & reg::uart::RXFIFO_CNT_MASK;
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
            // AFTER the drain: clearing first would drop the notification for a byte that
            // arrived while this pass was running.
            r32(win + reg::uart::OFF_INT_CLR) =
                reg::uart::RXFIFO_FULL_INT | reg::uart::RXFIFO_TOUT_INT;
        }

        void fill_tx()
        {
            unsigned char b = 0;
            while (tx_slot_free() != 0u and kos_byte_ring_pop_one(&sh->tx, &b) == 1)
            {
                r32(win + reg::uart::OFF_FIFO) = b;
                // The latch survives the FIFO passing the threshold, so it is dropped here
                // rather than once at the end of the burst.
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

    void c6uart_irq_thread(void* arg)
    {
        kickos::uart::Shared* sh = static_cast<kickos::uart::Shared*>(arg);
        C6Uart dev;
        dev.sh = sh;
        // No kos_periph_enable: the PMP grant carries the window and arch_init's HP_APM
        // REE0 permit already covers the UART0 block, so nothing bus-side gates it.
        dev.win = mmap::UART0_BASE;
        (void)dev.configure(115200u, 8u, KOS_UART_PARITY_NONE, 1u);
        dev.win_puts("[c6uart] device up (IRQ TX/RX)\n");
        kickos::uart::irq_loop(dev, sh); // parks in irq_wait; never returns
    }

    // Not kickos::uart::serve_loop: this endpoint carries TWO protocols. A kos_call is a
    // kos_uart_req frame, a plain send is raw console bytes, and serve_one refuses a plain
    // send, which for a console would silently swallow every print.
    void c6uart_service_thread(void* arg)
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
            // A plain send has no reply, so a short accept can be neither reported to the
            // sender nor retried by it: console_write_all carries that retry here instead,
            // and staying out of kos_recv until the ring has taken everything paces a
            // producer that outruns the 115200 baud wire.
            uint32_t const took =
                kickos::uart::console_write_all(sh, msg, static_cast<uint32_t>(n));
            sh->stats.tx_dropped += static_cast<uint32_t>(n) - took;
        }
        kos_exit(0);
    }
}

extern "C"
{

int c6uart_console_start(struct kos_service_cfg const* cfg)
{
    if (cfg == nullptr or cfg->kind != KOS_SVC_CONSOLE)
    {
        kos::print("[c6uart] ERROR: bad or non-console service cfg\n");
        return -1;
    }
    // The class is hard-wired to UART0 (the console), so a cfg naming another window would
    // grant one block and poke another.
    if (cfg->mmio_base != mmap::UART0_BASE)
    {
        kos::print("[c6uart] ERROR: cfg mmio_base is not UART0\n");
        return -1;
    }

    // 1. The shared block: ONE power-of-two, naturally-aligned allocation, because the RAM
    //    arm of the grant predicate demands it of every caller including this one.
    void* blk = kos_ram_alloc(kickos::uart::KOS_UART_BLOCK_SIZE);
    if (blk == nullptr)
    {
        kos::print("[c6uart] ERROR: arena cannot spare the ring block\n");
        return -1;
    }
    // Reach it before writing it: kos_ram_alloc hands back arena memory but grants nothing,
    // and under enforcement root's own region set does not cover the arena.
    if (kos_mem_self_grant(blk, kickos::uart::KOS_UART_BLOCK_SIZE) != 0)
    {
        kos::print("[c6uart] ERROR: mem_self_grant of the ring block refused\n");
        return -1;
    }
    g_shared = static_cast<kickos::uart::Shared*>(blk);
    kickos::uart::shared_init(g_shared);

    kos_cap_t ep = KOS_CAP_NONE;
    if (kos_endpoint_create(&ep) != 0)
    {
        kos::print("[c6uart] ERROR: endpoint_create failed\n");
        return -1;
    }

    // 2. Relinquish the kernel UART. This is also what frees the grouped UART0 line: the
    //    kernel's TX ring holds it via irq_attach, and irq_claim below would be refused
    //    while it does. On return the kernel console path is dark and drained.
    if (kos_console_publish(ep) != 0)
    {
        kos::print("[c6uart] ERROR: console_publish failed\n");
        kos_handle_close(ep);
        return -1;
    }

    // 3. The line, claimed HERE because minting needs KOS_AUTH_IRQ and both driver threads
    //    run at authority 0. LEVEL: the UART source stays asserted until the driver clears
    //    the latch, so the rearm must discard a stale pending first. It comes back MASKED:
    //    the IRQ thread's first wait arms it, in the thread that will consume the event.
    kos_cap_t irq = KOS_CAP_NONE;
    if (kos_irq_claim(c6irq::UART0_TX_LINE, KOS_IRQ_LEVEL, &irq) != 0)
    {
        kos::print("[c6uart] ERROR: irq_claim failed\n");
        kos_handle_close(ep);
        return -1;
    }

    // 4. The IRQ thread: the register window (R|W|DEV), the line (WAIT) and the shared
    //    block. Strictly ABOVE the service thread.
    kos_cap_grant const irq_caps[1] = {{irq, KOS_CAP_WAIT}};
    auto const irqt = kos::thread::spawn(c6uart_irq_thread, g_shared, "c6uartirq",
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
        kos::print("[c6uart] ERROR: IRQ thread spawn failed\n");
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
            kos::print("[c6uart] ERROR: IRQ thread never reached its loop\n");
            return -1;
        }
        waited++;
        kos_sleep_ns(READY_WAIT_NS);
    }

    // 5. The service thread: the endpoint (WAIT) and the SAME line as the DOORBELL (SIGNAL
    //    only). SIGNAL is a pure post on the binding, not a raise at the controller, so
    //    this thread starts a transfer without touching a register it does not own. No MMIO
    //    window, because a DEV window has exactly one holder.
    kos_cap_grant const svc_caps[2] = {{ep, KOS_CAP_WAIT}, {irq, KOS_CAP_SIGNAL}};
    auto const svct = kos::thread::spawn(c6uart_service_thread, g_shared, cfg->name,
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
        kos::print("[c6uart] ERROR: service thread spawn failed\n");
        return -1;
    }

    // Root's own line cap goes: with the two driver threads the only holders, the line
    // returns to the pool when BOTH die.
    kos_handle_close(irq);

    return kickos::driver::console_handover_finish(
        ep, "[c6uart] ERROR: driver died during bring-up\n");
}

}
