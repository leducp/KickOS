// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See <rxsci.h> for the source/thread map and the ordering rules; register facts come
// from the chip's clean-room headers (REGDIR).

#include "rxsci.h"

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/driver_bringup.h> // console_handover_finish
#include <kickos/sys/errno.h>
#include <kickos/sys/uart_service.h>

#include "mmap.h"
#include "regs/sci.h"

#include <stdint.h>

namespace mmap = kickos::rx::mmap;
namespace sci = kickos::rx::reg::sci;

namespace
{
    // Both are dedicated SCI6 vectors with their own INTB slot
    // (arch/rx/chip/rx72m/startup.S) and both are EDGE-detected: the ICU clears IRn on
    // acceptance, and a raise taken while the line is masked latches and redelivers on
    // the rearm.
    constexpr int SCI6_TXI_LINE = 87;
    constexpr int SCI6_RXI_LINE = 86;

    // One RX MPU page (16 bytes, arch_mpu_min_region), covering SMR..SEMR. The SCI6 base
    // is 16-byte aligned, so the window is exactly encodable.
    constexpr uint32_t SCI6_WINDOW = 16;

    // Per-byte cap on the first-light poll, so a channel that never reports TDRE costs a
    // delay rather than the IRQ thread.
    constexpr uint32_t TX_SPIN_MAX = 1000000u;

    // Root's wait for the IRQ thread to finish its device bring-up, in 1 ms steps.
    constexpr uint32_t READY_WAIT_MAX = 500u;
    constexpr uint64_t READY_WAIT_NS = 1000000u;

    // Bench instrumentation. Nothing under this knob may write TDR: with TIE armed that
    // re-raises the line service_irq is servicing and the pass never terminates
    // (regs/sci.h, TXI generation).
#ifndef RXSCI_TRACE
#define RXSCI_TRACE 0
#endif

// Bring-up bisect: leave the SCI6 receiver disabled. See configure().
#ifndef RXSCI_NO_RX
#define RXSCI_NO_RX 0
#endif

// Drop witness on the kernel diag LED (P80, PORT8), which shares nothing with SCI6 and so
// still reports once the console is silent. Latched ON the first time a console write
// gives up with bytes unaccepted, and never cleared after bring-up.
#ifndef RXSCI_LED_TRACE
#define RXSCI_LED_TRACE 0
#endif

    // Cap slots for the RX relay, in the order its spawn grants them: the line it waits
    // on first, the line it rings second.
    enum
    {
        RXSCI_CAP_RELAY_LINE = KOS_SPAWN_DELEGATED_CAP0,
        RXSCI_CAP_RELAY_DOORBELL = KOS_SPAWN_DELEGATED_CAP0 + 1
    };

    kickos::uart::Shared* g_shared = nullptr;

    // Every method touches the granted SCI6 window: IRQ thread only.
    struct Sci6Uart
    {
        kickos::uart::Shared* sh;

        static volatile uint8_t& r8(uintptr_t a)
        {
            return *reinterpret_cast<volatile uint8_t*>(a);
        }

        // The baud divisor is NOT reprogrammed: rewriting BRR on a live channel corrupts
        // the frame in flight, so the achieved rate is reported instead of the requested
        // one.
        //
        // ONE WRITE for the whole control word, the manual's own init order (Fig.42.11
        // step [7] p.2232: "Set the SCR.TE or RE bit to 1. At this time, also set the
        // SCR.TIE and RIE bits").
        //
        // TIE IS NOT SET HERE: the first drain arms it in the same pass that writes TDR
        // (RULE T1), which is the only ordering that starts TX at all.
        //
        // RXSCI_NO_RX drops RE and RIE. sci6_console_init leaves SCR = TE, so this is the
        // first time RXD6 is enabled on the board.
        uint32_t configure(uint32_t baud, uint8_t, uint8_t, uint8_t)
        {
            (void)baud;
#if RXSCI_NO_RX
            r8(sci::SCR) = static_cast<uint8_t>(sci::SCR_TE);
#else
            r8(sci::SCR) = static_cast<uint8_t>(sci::SCR_RIE | sci::SCR_TE | sci::SCR_RE);
#endif
            return sci::BAUD_115200_ACTUAL;
        }

        // Direct-to-register diagnostic, not stdio and not the ring. Callable ONLY with
        // TIE clear: with TIE armed a TDR write re-raises TXI (regs/sci.h), so this
        // belongs immediately after configure() and nowhere else.
        void win_puts(char const* s)
        {
            for (; *s != '\0'; s++)
            {
                for (uint32_t i = 0; i < TX_SPIN_MAX; i++)
                {
                    if ((r8(sci::SSR) & sci::SSR_TDRE) != 0)
                    {
                        r8(sci::TDR) = static_cast<uint8_t>(*s);
                        break;
                    }
                }
            }
        }

        // TEND, not TDRE: TDRE says the holding register is free, TEND says the shift
        // register has finished and nothing is queued behind it (UM sec.42.2.11 p.2171).
        bool tx_idle() const { return (r8(sci::SSR) & sci::SSR_TEND) != 0; }

        // Idempotent, and it belongs in the same pass as the TDR write (RULE T1): arming
        // TIE while TE is already 1 raises nothing (UM sec.42.12.2(1) p.2308), so the TDR
        // write is the only thing that can start the chain.
        void tx_irq_enable()
        {
            uint8_t const scr = r8(sci::SCR);
            if ((scr & sci::SCR_TIE) == 0)
            {
                r8(sci::SCR) = static_cast<uint8_t>(scr | sci::SCR_TIE);
            }
        }

        // Clearing TIE is the only lever that quiesces TXI: the tier-1 rearm unmasks on
        // every kos_irq_wait, so IER0A.IEN7 gates nothing across a wait. Callable only
        // with the ring empty, because it also discards the request the SCI retains
        // internally (UM sec.42.12.1(1) p.2308).
        void tx_irq_disable()
        {
            uint8_t const scr = r8(sci::SCR);
            if ((scr & sci::SCR_TIE) != 0)
            {
                r8(sci::SCR) = static_cast<uint8_t>(scr & ~sci::SCR_TIE);
                // MANDATORY read-back: an I/O write is posted and the next instruction
                // can run before it lands (UM sec.5 p.210), and sec.42.14.8 p.2317 and
                // sec.15.7.2 p.545 spell out this sequence for SCR.TIE. The caller
                // returns straight into kos_irq_wait, which unmasks the line, so an
                // unlanded TIE=0 rearms into a live source. The IR087 clear those
                // procedures also require is the kernel's (the ICU is a reserved block),
                // so a request latched during this window costs one spurious wake.
                while ((r8(sci::SCR) & sci::SCR_TIE) != 0)
                {
                }
            }
        }

        // Read the error flags once and clear the ones that were set, in the order the
        // manual requires: read RDR first (Fig.42.21 step [6] p.2243; an overrun is not
        // recoverable without it), then write 0 to exactly the bits that read 1 while
        // writing 1 to the TDRE/RDRF positions (Notes 1 and 2, p.2171). Storing back the
        // byte just read would violate Note 2; storing 0 would too.
        void clear_errors(uint8_t ssr)
        {
            uint8_t const seen = static_cast<uint8_t>(ssr & sci::SSR_ERRORS);
            if (seen == 0)
            {
                return;
            }
            if ((seen & sci::SSR_ORER) != 0)
            {
                sh->stats.rx_overrun++;
            }
            if ((seen & sci::SSR_FER) != 0)
            {
                sh->stats.rx_framing++;
            }
            if ((seen & sci::SSR_PER) != 0)
            {
                sh->stats.rx_parity++;
            }
            // Discard the offending byte, which also frees the buffer. Through a named
            // volatile: a cast to void would NOT access the register at all.
            uint8_t const dropped = r8(sci::RDR);
            (void)dropped;
            r8(sci::SSR) = static_cast<uint8_t>((ssr & ~seen) | sci::SSR_TDRE
                                                | sci::SSR_RDRF);
        }

        // A wake is not proof of a hardware event (the doorbell is a pure post and the RX
        // relay rings the same bell), so finding nothing asserted must be harmless.
        //
        // ERRORS FIRST: reception is stopped while any of ORER/FER/PER is 1
        // (UM sec.42.3.9 p.2241), so draining RX before clearing them would read a channel
        // that cannot receive and leave it dead.
        void service_irq()
        {
            uint8_t const ssr = r8(sci::SSR);
            clear_errors(ssr);

            // Reading RDR is what clears RDRF, so the loop terminates on its own. A full
            // RX ring drops the NEWEST byte and counts it; refusing to read the device
            // instead would convert a counted software overflow into a hardware overrun,
            // which stops reception entirely.
            while ((r8(sci::SSR) & sci::SSR_RDRF) != 0)
            {
                unsigned char const b = static_cast<unsigned char>(r8(sci::RDR));
                if (kos_byte_ring_push(&sh->rx, &b, 1) == 0u)
                {
                    sh->stats.rx_dropped++;
                }
                else
                {
                    sh->stats.rx_bytes++;
                }
            }

            // TIE IS ARMED BEFORE TDRE IS OBSERVED, and the order is the whole race (RULE
            // T1): the only raise this driver can use is a TDR -> TSR transfer taken with
            // TIE ALREADY 1 (UM sec.42.12.2(1) p.2308; setting TIE afterwards raises
            // nothing). Reading TDRE first leaves a window in which the pending transfer
            // completes with TIE still 0, and the pass then arms a source that will never
            // fire again: ring non-empty, TDRE 1, TIE 1, no edge left.
            unsigned char b = 0;
            while (kos_byte_ring_used(&sh->tx) != 0u)
            {
                tx_irq_enable();
                if ((r8(sci::SSR) & sci::SSR_TDRE) == 0)
                {
                    break; // TDR busy: the transfer in flight is the next raise
                }
                if (kos_byte_ring_pop_one(&sh->tx, &b) != 1)
                {
                    break;
                }
                r8(sci::TDR) = static_cast<uint8_t>(b);
            }
            if (kos_byte_ring_used(&sh->tx) == 0u)
            {
                tx_irq_disable();
            }
        }
    };

    void uart_irq_thread(void* arg)
    {
        kickos::uart::Shared* sh = static_cast<kickos::uart::Shared*>(arg);
        Sci6Uart dev;
        dev.sh = sh;
        (void)dev.configure(0u, 8u, KOS_UART_PARITY_NONE, 1u);
        dev.win_puts("[rxsci] device up (IRQ TX/RX)\n");
        kickos::uart::irq_loop(dev, sh); // parks in irq_wait; never returns
    }

    // RX line relay: owns no register, and turns an RXI6 wake into a post on the IRQ
    // thread's own binding. Rearming before the byte has been read is sound only because
    // RXI6 is EDGE, so the next raise is a genuinely new byte. A LEVEL source (TEI6/ERI6)
    // must never be relayed this way: the relay cannot clear the peripheral flag, so it
    // would rearm into a still-asserted source and spin.
    void uart_rx_relay_thread(void*)
    {
        while (true)
        {
            if (kos_irq_wait(RXSCI_CAP_RELAY_LINE) != 0)
            {
                break; // the cap went away: no line left to relay
            }
            // No LED here: it is the console-write drop witness and nothing else.
            (void)kos_irq_notify(RXSCI_CAP_RELAY_DOORBELL);
        }
        kos_exit(0);
    }

    void console_write(kickos::uart::Shared* sh, unsigned char const* buf, uint32_t n)
    {
        uint32_t const took = kickos::uart::console_write_all(sh, buf, n);
        if (took >= n)
        {
            return;
        }
        sh->stats.tx_dropped += n - took;
#if RXSCI_LED_TRACE
        kos_kernel_diag_led_set(1); // latched: the pump gave up with bytes unaccepted
#endif
    }

    // Not kickos::uart::serve_loop: kos_console_publish routes libc stdout to cap 0 as a
    // PLAIN SEND of raw bytes with no kos_uart_req framing, and serve_one discards a
    // reply-less message.
    void uart_service_thread(void* arg)
    {
        kickos::uart::Shared* sh = static_cast<kickos::uart::Shared*>(arg);
        unsigned char msg[KOS_EP_MSG_MAX];
        while (true)
        {
            struct kos_recv_info info;
            long const n = kos_recv(kickos::uart::KOS_UART_CAP_EP, msg, sizeof(msg), &info);
            if (n < 0)
            {
                break; // endpoint dead (EPIPE) or a bad cap: let the bring-up respawn us
            }
            if (info.reply_cap < 0)
            {
                if (n == 0)
                {
                    (void)kickos::uart::console_flush(sh); // zero-length plain send == flush
                    continue;
                }
                console_write(sh, msg, static_cast<uint32_t>(n));
                continue;
            }
            kickos::uart::serve_one(sh, msg, static_cast<size_t>(n), info.reply_cap);
        }
        kos_exit(0);
    }
}

extern "C"
{

int rxsci_console_start(struct kos_service_cfg const* cfg)
{
    if (cfg == nullptr or cfg->kind != KOS_SVC_CONSOLE)
    {
        kos::print("[rxsci] ERROR: bad or non-console service cfg\n");
        return -1;
    }
    // The class addresses SCI6 through absolute constants, so a cfg naming a different
    // block would grant one peripheral and drive another.
    if (cfg->mmio_base != 0u and cfg->mmio_base != mmap::SCI6)
    {
        kos::print("[rxsci] ERROR: cfg mmio_base is not the SCI6 block\n");
        return -1;
    }

    // 1. The shared ring block: ONE power-of-two, naturally-aligned allocation, because
    //    the RAM arm of the grant predicate demands it of every caller including root.
    void* blk = kos_ram_alloc(kickos::uart::KOS_UART_BLOCK_SIZE);
    if (blk == nullptr)
    {
        kos::print("[rxsci] ERROR: arena cannot spare the ring block\n");
        return -1;
    }
    // Reach it before writing it: kos_ram_alloc hands back arena memory but grants
    // nothing, and under enforcement root's own region set does not cover the arena.
    if (kos_mem_self_grant(blk, kickos::uart::KOS_UART_BLOCK_SIZE) != 0)
    {
        kos::print("[rxsci] ERROR: mem_self_grant of the ring block refused\n");
        return -1;
    }
    g_shared = static_cast<kickos::uart::Shared*>(blk);
    kickos::uart::shared_init(g_shared);

    int const ep = kos_endpoint_create();
    if (ep < 0)
    {
        kos::print("[rxsci] ERROR: endpoint_create failed\n");
        return -1;
    }

    // 2. PUBLISH BEFORE CLAIM: the publish runs console_tx_deinit, which detaches the
    //    kernel ring's handler from vector 87 and masks the line. irq_claim refuses a
    //    line while any handler but the default is attached (INVARIANT H2), and the INTB
    //    slot only starts routing raises to a claimed line once that ring is disarmed.
    if (kos_console_publish(ep) != 0)
    {
        kos::print("[rxsci] ERROR: console_publish failed\n");
        kos_handle_close(ep);
        return -1;
    }

    // 3. The two lines. Claimed HERE because minting needs KOS_AUTH_IRQ and every driver
    //    thread runs at authority 0. Both come back MASKED: the waiting thread's first
    //    irq_wait arms the line, in the thread that will consume the event.
    int const txi = kos_irq_claim(SCI6_TXI_LINE, KOS_IRQ_EDGE);
    if (txi < 0)
    {
        kos_handle_close(ep);
        kos::print("[rxsci] ERROR: irq_claim of TXI6 failed\n");
        return -1;
    }
    int const rxi = kos_irq_claim(SCI6_RXI_LINE, KOS_IRQ_EDGE);
    if (rxi < 0)
    {
        kos_handle_close(txi);
        kos_handle_close(ep);
        kos::print("[rxsci] ERROR: irq_claim of RXI6 failed\n");
        return -1;
    }

    // 4. The IRQ thread: the TX line (WAIT), the ring block and the SCI6 window, of which
    //    it is the ONLY holder. Strictly above the service thread.
    kos_cap_grant const irq_caps[1] = {{txi, KOS_CAP_WAIT}};
    int const irqt = kos::thread::spawn(uart_irq_thread, g_shared, "rxsciirq",
                                        static_cast<uint8_t>(cfg->prio + 1),
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                        /*mem=*/g_shared,
                                        kickos::uart::KOS_UART_BLOCK_SIZE,
                                        /*stack=*/nullptr, /*stack_size=*/0,
                                        /*mmio=*/reinterpret_cast<void*>(mmap::SCI6),
                                        SCI6_WINDOW, irq_caps, 1);
    if (irqt < 0)
    {
        kos_handle_close(rxi);
        kos_handle_close(txi);
        kos_handle_close(ep);
        kos::print("[rxsci] ERROR: IRQ thread spawn failed\n");
        return -1;
    }

    // 5. The RX relay: the RX line (WAIT) and the TX line as its doorbell (SIGNAL). No
    //    window and no ring.
    kos_cap_grant const relay_caps[2] = {{rxi, KOS_CAP_WAIT}, {txi, KOS_CAP_SIGNAL}};
    int const relayt = kos::thread::spawn(uart_rx_relay_thread, nullptr, "rxscirx",
                                          static_cast<uint8_t>(cfg->prio + 1),
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                          /*mem=*/nullptr, /*mem_size=*/0,
                                          /*stack=*/nullptr, /*stack_size=*/0,
                                          /*mmio=*/nullptr, 0, relay_caps, 2);
    if (relayt < 0)
    {
        kos_handle_close(rxi);
        kos_handle_close(txi);
        kos_handle_close(ep);
        kos::print("[rxsci] ERROR: RX relay spawn failed\n");
        return -1;
    }

    // 6. Wait for the IRQ thread's own device bring-up, STRICTLY BEFORE THE SERVICE
    //    THREAD EXISTS: root is still the endpoint's ONLY receiver holder here, so the
    //    close below takes recv_holders to zero and reclaims the console, which is what
    //    makes this timeout reportable. The handover probe proves only that the SERVICE
    //    thread is serving. Sleeping rather than spinning because the IRQ thread may sit
    //    below root's priority.
    uint32_t waited = 0;
    while (g_shared->ready == 0u)
    {
        if (waited >= READY_WAIT_MAX)
        {
            kos_handle_close(rxi);
            kos_handle_close(txi);
            kos_handle_close(ep);
            kos::print("[rxsci] ERROR: IRQ thread never reached its loop\n");
            return -1;
        }
        waited++;
        kos_sleep_ns(READY_WAIT_NS);
    }

    // 7. The service thread: the endpoint (WAIT) and the TX line as the TX doorbell
    //    (SIGNAL only). SIGNAL is a pure post on the binding, not a raise at the
    //    controller, so this thread starts a transfer without touching a register.
    kos_cap_grant const svc_caps[2] = {{ep, KOS_CAP_WAIT}, {txi, KOS_CAP_SIGNAL}};
    int const svct = kos::thread::spawn(uart_service_thread, g_shared, cfg->name,
                                        cfg->prio, KOS_POLICY_FIFO, 0,
                                        /*privileged=*/false,
                                        /*mem=*/g_shared,
                                        kickos::uart::KOS_UART_BLOCK_SIZE,
                                        /*stack=*/nullptr, /*stack_size=*/0,
                                        /*mmio=*/nullptr, 0, svc_caps, 2);
    if (svct < 0)
    {
        kos_handle_close(rxi);
        kos_handle_close(txi);
        kos_handle_close(ep);
        kos::print("[rxsci] ERROR: service thread spawn failed\n");
        return -1;
    }

#if RXSCI_TRACE
    // TX-path probe from ROOT, bypassing the service thread: root holds the ring block
    // (self-granted) and still holds txi with SIGNAL. A 'P' on the wire proves ring +
    // doorbell + drain.
    {
        unsigned char const p = 'P';
        (void)kos_byte_ring_push(&g_shared->tx, &p, 1);
        (void)kos_irq_notify(txi);
    }
#endif

#if RXSCI_LED_TRACE
    kos_kernel_diag_led_set(0); // clear the drop witness before any client can print
#endif

    // Root's own line caps go: with the driver threads the only holders, a line returns to
    // the pool when they die.
    kos_handle_close(rxi);
    kos_handle_close(txi);
    return kickos::driver::console_handover_finish(
        ep, "[rxsci] ERROR: driver did not answer the handover probe\n");
}

}
