// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/UART0 buffered IRQ-driven userspace UART driver (see k64uartirq.h).
//
// Register facts are from the K64 Sub-Family Reference Manual (K64P144M120SF5RM), cited
// per non-obvious bit below. Three of them shape the whole file:
//
//   1. TDRE (S1 bit 7) is a WATERMARK comparison against TWFIFO.TXWATER, latched but
//      re-asserted while the condition holds (RM 52.3.5: "the TDRE reasserts until the
//      watermark has been exceeded"), and it resets SET with nothing ever transmitted
//      (S1 reset 0xC0). So arming TIE on an idle channel raises IMMEDIATELY here, and
//      RULE T1 is satisfied but not load-bearing on this part.
//   2. The RECEIVE ERROR flags are on a DIFFERENT vector. UART0 status (TDRE/TC/RDRF) is
//      IRQ 31; OR/NF/FE/PF are IRQ 32 (RM 3.2.2.3 Table 3-5), which nothing claims. They
//      still appear in the same S1 this driver reads, and they must be cleared here: FE
//      inhibits all further reception until cleared and OR blocks RDRF from asserting
//      (RM 52.3.5), so an unclaimed error line plus an ignoring handler is a permanently
//      dead receiver.
//   3. C2 is writable at any time (RM 52.3.4, first sentence), so TIE/RIE toggle on a
//      live channel with no disable-reconfigure-enable dance.
//
// HARD RULE (design D7): NO libc stdio anywhere in this file. printf/puts route through
// _write -> kos_send(cap 0) -> this driver's own endpoint, and the service thread holds
// the only WAIT cap on it, so a self-send never returns. Diagnostics use kos::print (the
// kernel debug / RTT path), which on a board with no RTT is mute rather than fatal.

#include "k64uartirq.h"

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/io/mmio.h>            // r8 (this register file is byte-mapped)
#include <kickos/sys/driver_bringup.h> // console_handover_finish
#include <kickos/sys/uart_service.h>   // Shared, shared_init, irq_loop, serve_one

#include <irq.h>        // UART0_RXTX_IRQ, the chip's own vector table
#include <regs/uart.h>  // the chip's shared UART register map, not a local copy
#include <uart_class.h> // Rule 6 class leaf: the shared transmit-ready read

#include <stddef.h>
#include <stdint.h>

namespace
{
    namespace ru = kickos::mk64f::reg::uart;

    // Both threads reach this through their thread ARG. It sits inside the ONE
    // power-of-two naturally-aligned allocation the RAM arm of grant_region_admissible
    // requires, which is also the grant both threads get.
    struct Ctx
    {
        uintptr_t win;
        uint32_t baud;
        kickos::uart::Shared sh;
    };

    static_assert(sizeof(Ctx) <= kickos::uart::KOS_UART_BLOCK_SIZE,
                  "the driver context must fit the one 1 KiB power-of-two grant");

    // Bounded so a channel that never reports TDRE costs a delay rather than the IRQ
    // thread. Far above any real per-byte wait.
    constexpr uint32_t TX_IDLE_SPIN_MAX = 1000000u;

    // RX bytes moved per pass. Exiting early with RDRF still asserted loses nothing: the
    // line is a LEVEL binding, so the source re-asserts and the rearm redelivers. The
    // bound is what stops a fault that leaves a status flag permanently set from turning
    // one wake into a hang.
    constexpr uint32_t RX_BURST_MAX = 64u;

    // Root's wait for the IRQ thread to finish its device bring-up, in 1 ms steps.
    constexpr uint32_t READY_WAIT_MAX = 500u;
    constexpr uint64_t READY_WAIT_NS = 1000000u;

    // Every method touches the granted window: IRQ thread only.
    struct Uart
    {
        uintptr_t win;
        kickos::uart::Shared* sh;

        bool tx_idle() const
        {
            uint8_t const s1 = r8(win + ru::S1_OFFSET);
            return (s1 & (ru::S1_TDRE | ru::S1_TC)) == (ru::S1_TDRE | ru::S1_TC);
        }

        void tx_irq_enable()
        {
            r8(win + ru::C2_OFFSET) =
                static_cast<uint8_t>(r8(win + ru::C2_OFFSET) | ru::C2_TIE);
        }

        void tx_irq_disable()
        {
            r8(win + ru::C2_OFFSET) =
                static_cast<uint8_t>(r8(win + ru::C2_OFFSET) & ~ru::C2_TIE);
        }

        // Returns the achieved baud, or 0 for a frame format this UART cannot express.
        // Call once, before the IRQ loop: a divisor change mid-frame corrupts the wire.
        uint32_t configure(uint32_t baud, uint8_t data_bits, uint8_t parity,
                           uint8_t stop_bits)
        {
            if (baud == 0u)
            {
                return 0u;
            }
            if (stop_bits != 1u and stop_bits != 2u)
            {
                return 0u;
            }
            bool const has_parity = (parity != KOS_UART_PARITY_NONE);
            if (parity != KOS_UART_PARITY_NONE and parity != KOS_UART_PARITY_EVEN
                and parity != KOS_UART_PARITY_ODD)
            {
                return 0u;
            }
            // C1.M=0 is an 8-bit frame TOTAL, so an enabled parity bit REPLACES the
            // eighth data bit (RM 52.4.4.1 Table 52-11 and the RM 52.3.8 note). 8 data
            // bits plus parity is therefore the 9-bit frame, and 7-bit-no-parity does not
            // exist on this part at all.
            uint8_t c1 = 0u;
            bool expressible = false;
            if (data_bits == 8u and not has_parity)
            {
                expressible = true;
            }
            if (data_bits == 7u and has_parity)
            {
                expressible = true;
            }
            if (data_bits == 8u and has_parity)
            {
                expressible = true;
                c1 = static_cast<uint8_t>(c1 | ru::C1_M);
            }
            if (not expressible)
            {
                return 0u;
            }
            if (has_parity)
            {
                c1 = static_cast<uint8_t>(c1 | ru::C1_PE);
            }
            if (parity == KOS_UART_PARITY_ODD)
            {
                c1 = static_cast<uint8_t>(c1 | ru::C1_PT);
            }
            uint8_t sbns = 0u;
            if (stop_bits == 2u)
            {
                sbns = ru::BDH_SBNS;
            }

            // The channel goes off first for two independent reasons: PFIFO is writable
            // only while TE and RE are clear (RM 52.3.16), and a byte still in the shifter
            // from the kernel's last banner would be truncated on the wire.
            for (uint32_t i = 0; i < TX_IDLE_SPIN_MAX; i++)
            {
                if (tx_idle())
                {
                    break;
                }
            }
            r8(win + ru::C2_OFFSET) = 0u;

            // Re-forced, not assumed: a RESTARTED driver inherits whatever the previous
            // instance left here, and each of these destroys the wire silently rather than
            // failing.
            r8(win + ru::PFIFO_OFFSET) = 0u; // FIFOs off, buffers depth 1 (RM 52.3.16)
            // RM 52.3.16 requires a flush immediately after any PFIFO change, and it is
            // also what re-aligns a receive buffer left misaligned by a previous
            // instance's clear-by-reading-an-empty-D (the RM 52.3.5 note).
            r8(win + ru::CFIFO_OFFSET) =
                static_cast<uint8_t>(ru::CFIFO_TXFLUSH | ru::CFIFO_RXFLUSH);
            r8(win + ru::MODEM_OFFSET) = 0u; // TXCTSE: an absent CTS stalls every byte
            r8(win + ru::C3_OFFSET) = 0u;    // TXINV: an inverted TX corrupts every frame
            r8(win + ru::S2_OFFSET) = 0u;    // LBKDE: prevents RDRF from ever setting
            r8(win + ru::C5_OFFSET) = 0u;    // TDMAS/RDMAS: a DMA request in place of ours
            r8(win + ru::IR_OFFSET) = 0u;    // IREN: infrared modulation on the TX pin
            r8(win + ru::C7816_OFFSET) = 0u; // ISO-7816 framing

            // baud = clk / (16 x (SBR + BRFA/32)), RM 52.4.3. UART0 is CORE-clocked while
            // UART2..4 are bus-clocked (RM 5.7.10), so the divisor must come from the
            // per-block clock oracle, not from a chip-wide constant.
            uint32_t achieved = baud;
            uint32_t const clk = kos_periph_clock_hz(win);
            if (clk == 0u)
            {
                // The oracle does not model this block: keep the kernel's divisor, and
                // re-latch it so SBNS still lands. A BDH write is buffered and takes
                // effect only when BDL is written (RM 52.3.1).
                uint8_t const bdh = r8(win + ru::BDH_OFFSET);
                uint8_t const bdl = r8(win + ru::BDL_OFFSET);
                r8(win + ru::BDH_OFFSET) =
                    static_cast<uint8_t>((bdh & ru::BDH_SBR_MASK) | sbns);
                r8(win + ru::BDL_OFFSET) = bdl;
            }
            else
            {
                uint32_t const sbr = clk / (16u * baud);
                if (sbr == 0u or sbr > 8191u)
                {
                    return 0u; // SBR 0 disables the generator (RM 52.3.1); 13 bits is the range
                }
                // BRFA is floor(fraction x 32) and the fraction is below 1, so this always
                // lands inside the 5-bit field.
                uint32_t const brfa = (clk * 2u) / baud - sbr * 32u;
                r8(win + ru::BDH_OFFSET) =
                    static_cast<uint8_t>(((sbr >> 8) & ru::BDH_SBR_MASK) | sbns);
                r8(win + ru::BDL_OFFSET) = static_cast<uint8_t>(sbr & 0xFFu);
                r8(win + ru::C4_OFFSET) = static_cast<uint8_t>(brfa & ru::C4_BRFA_MASK);
                achieved = (clk * 2u) / (sbr * 32u + brfa);
            }

            r8(win + ru::C1_OFFSET) = c1;
            // RIE on from here; TIE stays OFF until the ring holds a byte, because the
            // pass that arms it must also push (RULE T1). TCIE and ILIE stay clear so TC
            // and IDLE never assert this line, which keeps the demux to two conditions.
            r8(win + ru::C2_OFFSET) =
                static_cast<uint8_t>(ru::C2_TE | ru::C2_RE | ru::C2_RIE);
            return achieved;
        }

        // A wake is not proof of a hardware event (the doorbell is a pure post), so
        // finding nothing asserted must be harmless.
        void service_irq()
        {
            uint32_t moved = 0;
            while (moved < RX_BURST_MAX)
            {
                // Re-read per iteration, not once: S1 is the FIRST HALF of every clear
                // sequence on this part (RM 52.3.5, "read the status register followed by
                // a read or write to D"), and an error raised mid-burst must be seen in
                // the same pass. OR blocks RDRF, so a pass that leaves OR set gets no
                // further RX wake at all.
                uint8_t const s1 = r8(win + ru::S1_OFFSET);
                if ((s1 & ru::S1_RX_ERRORS) != 0u)
                {
                    count_rx_errors(s1);
                    // Through a named copy, not a cast to void: discarding a volatile
                    // lvalue does not perform the access at all, and the ACCESS is the
                    // second half of the clear sequence. The byte itself is discarded.
                    unsigned char const bad = r8(win + ru::D_OFFSET);
                    (void)bad;
                    // OR sets with the receive buffer EMPTY ("If the OR flag is set, no
                    // data is stored in the data buffer even if sufficient room exists",
                    // RM 52.3.5), so the mandated read above is then a read of an empty
                    // D, which "causes the FIFO pointers to become misaligned. A receive
                    // FIFO flush reinitializes the pointers" (RM 52.3.5 note). Without
                    // the flush the first overrun corrupts reception permanently. CFIFO
                    // is writable at any time (RM 52.3.17).
                    if ((s1 & ru::S1_RDRF) == 0u)
                    {
                        r8(win + ru::CFIFO_OFFSET) = ru::CFIFO_RXFLUSH;
                    }
                    moved++;
                    continue;
                }
                if ((s1 & ru::S1_RDRF) == 0u)
                {
                    break;
                }
                unsigned char const b = r8(win + ru::D_OFFSET);
                if (kos_byte_ring_push(&sh->rx, &b, 1u) == 0u)
                {
                    // The NEWEST byte is dropped and counted. Refusing to read the device
                    // instead would turn a counted software overflow into a hardware
                    // overrun plus a stuck level source, i.e. a storm rather than a loss.
                    sh->stats.rx_dropped++;
                }
                else
                {
                    sh->stats.rx_bytes++;
                }
                moved++;
            }
            drain_tx();
        }

        void count_rx_errors(uint8_t s1)
        {
            if ((s1 & ru::S1_OR) != 0u)
            {
                sh->stats.rx_overrun++;
            }
            if ((s1 & ru::S1_FE) != 0u)
            {
                sh->stats.rx_framing++;
            }
            if ((s1 & ru::S1_PF) != 0u)
            {
                sh->stats.rx_parity++;
            }
            // NF has no field in kos_uart_stats and is deliberately not folded into
            // rx_framing: it flags noise on a byte that framed correctly. It is still
            // cleared with the others.
        }

        // Direct-to-window diagnostic, not stdio and not the endpoint. Runs before the IRQ
        // loop and before any ring traffic, with TIE clear, so writing D cannot assert the
        // line.
        void win_puts(char const* s)
        {
            for (; *s != '\0'; s++)
            {
                for (uint32_t i = 0; i < TX_IDLE_SPIN_MAX; i++)
                {
                    if (kickos::mk64f::driver::uart0_tx_ready(win))
                    {
                        r8(win + ru::D_OFFSET) = static_cast<uint8_t>(*s);
                        break;
                    }
                }
            }
        }

        void drain_tx()
        {
            while (kickos::mk64f::driver::uart0_tx_ready(win))
            {
                unsigned char b = 0;
                if (kos_byte_ring_pop_one(&sh->tx, &b) != 1)
                {
                    break;
                }
                r8(win + ru::D_OFFSET) = b;
            }
            // Arming AFTER the push is what satisfies RULE T1: the first byte of a burst
            // leaves in this same pass, so nothing waits on an interrupt a transition-
            // triggered part would never raise from an idle channel.
            if (kos_byte_ring_used(&sh->tx) == 0u)
            {
                tx_irq_disable();
            }
            else
            {
                tx_irq_enable();
            }
        }
    };

    // A plain kos_send has no reply, so a short accept cannot be reported and the retry
    // has to live here. Its budget is bounded so a wedged channel cannot park every stdout
    // client forever.
    void console_write(kickos::uart::Shared* sh, unsigned char const* buf, uint32_t n)
    {
        uint32_t const took = kickos::uart::console_write_all(sh, buf, n);
        if (took >= n)
        {
            return;
        }
        sh->stats.tx_dropped += n - took;
    }

    void irq_thread(void* arg)
    {
        Ctx* ctx = static_cast<Ctx*>(arg);

        // Until this returns every register access below is supervisor-only and faults:
        // the AIPS slot resets with Supervisor-Protect set (RM 3.3.8.4, PACRN reset
        // 0x4444_4444; RM 20.2.3 for the SP field). Possession of the exact window base is
        // what authorises the call.
        if (kos_periph_enable(ctx->win) != 0)
        {
            kos::print("[k64uartirq] ERROR: periph_enable failed, UART0 unreachable\n");
            kos_exit(-1);
        }
        Uart dev;
        dev.win = ctx->win;
        dev.sh = &ctx->sh;
        if (dev.configure(ctx->baud, 8u, KOS_UART_PARITY_NONE, 1u) == 0u)
        {
            kos::print("[k64uartirq] ERROR: UART0 baud/frame unprogrammable\n");
            kos_exit(-1);
        }
        dev.win_puts("[k64uartirq] device up (IRQ TX/RX)\n");
        kickos::uart::irq_loop(dev, &ctx->sh); // parks in irq_wait; never returns
    }

    void service_thread(void* arg)
    {
        Ctx* ctx = static_cast<Ctx*>(arg);
        kickos::uart::Shared* sh = &ctx->sh;
        unsigned char msg[KOS_EP_MSG_MAX];
        while (true)
        {
            struct kos_recv_info info;
            long const n = kos_recv(kickos::uart::KOS_UART_CAP_EP, msg, sizeof(msg), &info);
            if (n < 0)
            {
                break; // endpoint dead (EPIPE) or a bad cap: let the bring-up respawn us
            }
            // A console client SENDS raw bytes; a UART client CALLS with a wire frame, and
            // serve_one returns without effect on a reply-less message.
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

int k64uartirq_console_start(struct kos_service_cfg const* cfg)
{
    if (cfg == nullptr or cfg->kind != KOS_SVC_CONSOLE)
    {
        kos::print("[k64uartirq] ERROR: bad or non-console service cfg\n");
        return -1;
    }

    void* const blk = kos_ram_alloc(kickos::uart::KOS_UART_BLOCK_SIZE);
    if (blk == nullptr)
    {
        kos::print("[k64uartirq] ERROR: arena cannot spare the ring block\n");
        return -1;
    }
    // Reach it before writing it: kos_ram_alloc reserves but grants NOTHING, and under
    // enforcement root's own region set does not cover the arena.
    if (kos_mem_self_grant(blk, kickos::uart::KOS_UART_BLOCK_SIZE) != 0)
    {
        kos::print("[k64uartirq] ERROR: mem_self_grant of the ring block refused\n");
        return -1;
    }
    Ctx* const ctx = static_cast<Ctx*>(blk);
    kickos::uart::shared_init(&ctx->sh);
    ctx->win = cfg->mmio_base;
    ctx->baud = cfg->hz;
    if (ctx->baud == 0u)
    {
        ctx->baud = 115200u;
    }

    int const ep = kos_endpoint_create();
    if (ep < 0)
    {
        kos::print("[k64uartirq] ERROR: endpoint_create failed\n");
        return -1;
    }

    // STRICTLY BEFORE THE CLAIM. The kernel's own console ring holds IRQ 31 until this
    // call runs console_tx_deinit, and irq_claim refuses any line whose handler is not
    // still the default (INVARIANT H2), so swapping these two lines yields -KOS_EBUSY
    // every boot. On return the kernel chip path is dark and drained.
    if (kos_console_publish(ep) != 0)
    {
        kos::print("[k64uartirq] ERROR: console_publish failed\n");
        kos_handle_close(ep);
        return -1;
    }

    // LEVEL, not EDGE: every source on this vector is a status flag that stays asserted
    // until the driver clears it at the peripheral, so the rearm must discard a latch that
    // survived from before that clear (design-m4.6-irq-driver.md section 5). Claimed by
    // root because the mint needs AUTH_IRQ and both driver threads run at authority 0; it
    // comes back MASKED, and the IRQ thread's first wait is what arms it.
    int const irq = kos_irq_claim(kickos::mk64f::irq::UART0_RXTX_IRQ, KOS_IRQ_LEVEL);
    if (irq < 0)
    {
        // CLOSE BEFORE PRINTING: the publish above already flipped the console to
        // USER_OWNED, where a kernel-console write is a bare DROP. Closing takes the
        // endpoint's last receiver holder to zero, which reclaims the console, so the
        // tag reaches the wire instead of vanishing.
        kos_handle_close(ep);
        kos::print("[k64uartirq] ERROR: irq_claim of UART0 status failed\n");
        return -1;
    }

    // The IRQ thread: the line (WAIT), the shared block and the REGISTER WINDOW. The
    // service thread below deliberately gets no window: a DEV window has exactly one
    // holder, so its spawn would be refused -KOS_EBUSY if it asked. Strictly above the
    // service thread.
    kos_cap_grant const irq_caps[1] = { { irq, KOS_CAP_WAIT } };
    int const irqt = kos::thread::spawn(
        irq_thread, ctx, "uartirq", static_cast<uint8_t>(cfg->prio + 1), KOS_POLICY_FIFO,
        /*quantum_ns=*/0, /*privileged=*/false, /*mem=*/ctx,
        kickos::uart::KOS_UART_BLOCK_SIZE, /*stack=*/nullptr, /*stack_size=*/0,
        /*mmio=*/reinterpret_cast<void*>(ctx->win), cfg->mmio_window, irq_caps, 1);
    if (irqt < 0)
    {
        kos_handle_close(irq);
        kos_handle_close(ep); // reclaims the console, so the tag below reaches the wire
        kos::print("[k64uartirq] ERROR: IRQ thread spawn failed\n");
        return -1;
    }

    // STRICTLY BEFORE THE SERVICE THREAD EXISTS, and that is what makes this timeout
    // reportable: root is still the endpoint's ONLY receiver holder here, so the close
    // below takes recv_holders to zero and reclaims the console. Spawning the service
    // thread first would pin recv_holders at one, leaving a dark console and clients
    // queueing into a ring nobody drains. The handover probe proves only that the SERVICE
    // thread is serving, and would succeed against a device that was never configured.
    // Sleeping rather than spinning because that thread may sit below root's priority.
    //
    // Every failure path from here on also CANCELS the IRQ thread, and closes BEFORE
    // cancelling so the note is already set when the cancelled thread's exit runs the
    // reclaim: the console is not given back while a live thread holds the register
    // window (kernel/init/console.cc). Cancellation is cooperative, so the one case it
    // cannot rescue is this very timeout with the IRQ thread wedged BEFORE its first
    // kos_irq_wait: it is marked, does not die, and the tag below is dropped.
    uint32_t waited = 0;
    while (ctx->sh.ready == 0u)
    {
        if (waited >= READY_WAIT_MAX)
        {
            kos_handle_close(irq);
            kos_handle_close(ep);
            (void)kos_thread_kill(irqt);
            kos::print("[k64uartirq] ERROR: IRQ thread never reached its loop\n");
            return -1;
        }
        waited++;
        kos_sleep_ns(READY_WAIT_NS);
    }

    // The service thread: the endpoint (WAIT) and the SAME line as the DOORBELL (SIGNAL
    // only). SIGNAL is a pure post on the binding, not a raise at the controller, which
    // is what lets this thread start a transfer without touching a register it cannot own.
    kos_cap_grant const svc_caps[2] = { { ep, KOS_CAP_WAIT }, { irq, KOS_CAP_SIGNAL } };
    int const svct = kos::thread::spawn(
        service_thread, ctx, cfg->name, cfg->prio, KOS_POLICY_FIFO, /*quantum_ns=*/0,
        /*privileged=*/false, /*mem=*/ctx, kickos::uart::KOS_UART_BLOCK_SIZE,
        /*stack=*/nullptr, /*stack_size=*/0, /*mmio=*/nullptr, 0, svc_caps, 2);
    if (svct < 0)
    {
        kos_handle_close(irq);
        kos_handle_close(ep);
        (void)kos_thread_kill(irqt); // frees the window, which is what gives the console back
        kos::print("[k64uartirq] ERROR: service thread spawn failed\n");
        return -1;
    }

    // Root's own line cap goes: with the two driver threads the only holders, the line
    // returns to the pool when BOTH die.
    kos_handle_close(irq);

    return kickos::driver::console_handover_finish(
        ep, "[k64uartirq] ERROR: driver died during bring-up\n", irqt);
}

}
