// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 IRQ-driven buffered UART console driver on USIC0 CH0 (see
// <kickos/driver/xmcuartirq.h>).
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference
// Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. "RM p.NN" are the manual's
// printed pages.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/xmcuartirq.h>

#include <kickos/io/mmio.h>            // r32
#include <kickos/sys/driver_bringup.h> // kickos::driver::console_handover_finish
#include <kickos/sys/service.h>        // kos_service_cfg (base/window/prio as data)
#include <kickos/sys/uart_service.h>   // Shared, shared_init, irq_loop, serve_one

#include <irq.h>        // kickos::xmc::irq::USIC0_SR0
#include <regs/usic.h>  // shared USIC register offsets + ASC bit fields
#include <usic_class.h> // Rule 6 class-driver leaf: shared USIC transmit-ready read

#include <stddef.h>
#include <stdint.h>

namespace
{
    namespace ru = kickos::xmc::reg::usic;

    // The channel's ONE privileged word. MODE must be RESTATED here, not just TBIEN: the
    // seam stores the whole 32-bit word absolutely and never read-modify-writes, so a bare
    // TBIEN would clear MODE[3:0] and DISABLE the channel. RIEN/AIEN stay clear, so no
    // receive source is armed.
    constexpr uint32_t CCR_WORD = ru::CCR_MODE_ASC | ru::CCR_TBIEN;

    // Both threads reach this through their thread ARG; it sits in the ONE granted block.
    struct Ctx
    {
        kickos::uart::Shared sh;
        uintptr_t win;
    };

    static_assert(sizeof(Ctx) <= kickos::uart::KOS_UART_BLOCK_SIZE,
                  "the U0C0 driver context must fit the 1 KiB shared grant");

    // Per-byte cap on the first-light poll, so a channel that never reports TDV clear
    // costs a delay rather than the IRQ thread.
    constexpr uint32_t TX_SPIN_MAX = 1000000u;

    // Root's wait for the IRQ thread to finish its device bring-up, in 1 ms steps.
    constexpr uint32_t READY_WAIT_MAX = 500u;
    constexpr uint64_t READY_WAIT_NS = 1000000u;

    // Every method touches the granted window: IRQ thread only.
    class ConsoleUart
    {
    public:
        void init(uintptr_t win, kickos::uart::Shared* sh)
        {
            win_ = win;
            sh_ = sh;
        }

        // Baud is kernel-owned: FDR/BRG are Write = PV with no U0C0 allowlist entry, so a
        // store here is discarded at the bus. The request is reported back unchanged.
        uint32_t configure(uint32_t baud, uint8_t, uint8_t, uint8_t) { return baud; }

        bool tx_idle() const
        {
            // PCR.TSTEN is set by the kernel's channel init, which is what makes BUSY the
            // real end-of-frame test; TCSR.TDV clears one frame early (RM p.18-70).
            return (r32(win_ + ru::off::PSR) & ru::PSR_BUSY) == 0u;
        }

        // Deliberately empty: TBIEN is written once by bring_up() and never gated. The
        // transmit-buffer event fires only when a word is loaded from TBUF into the shift
        // register, so an empty ring raises nothing and there is no storm to suppress.
        void tx_irq_enable() {}

        // A wake is not proof of a hardware event (the doorbell is a pure post), so
        // finding an empty ring here must be harmless.
        void service_irq() { drain_tx(); }

        // Returns false if the seam refused the CCR write or the bus dropped it.
        bool bring_up()
        {
            if (not priv_write_verify(ru::off::CCR, CCR_WORD))
            {
                return false;
            }
            // RULE T1: the pass that enables the TX interrupt must ALSO push the first
            // byte. The USIC transmit-buffer event is EDGE-PER-WORD, occurring when a word
            // is loaded from TBUF into the shift register, "with the transmit clock edge
            // that shifts out the first bit of a new data word" (RM 18.2.2.4 p.18-18;
            // ASC-specific p.18-64), so TBIEN on an idle channel raises NOTHING and a pass
            // that armed and then waited would hang forever. Every later burst is primed
            // the same way, by the doorbell-woken drain.
            //
            // The marker must precede that drain: the CCR word above has already set
            // TBIEN, so these polled writes raise transmit-buffer events, and only a pend
            // latched before the line's FIRST irq_wait is discarded.
            win_puts("[xmcuartirq] device up (IRQ TX)\n");
            drain_tx();
            return true;
        }

    private:
        // Direct-to-window diagnostic, not stdio and not the ring. Bounded per byte, so a
        // dead channel costs a delay rather than the thread.
        void win_puts(char const* s)
        {
            for (; *s != '\0'; s++)
            {
                for (uint32_t i = 0; i < TX_SPIN_MAX; i++)
                {
                    if (kickos::xmc::driver::usic_tx_ready(win_))
                    {
                        r32(win_ + ru::off::TBUF0) =
                            static_cast<uint32_t>(static_cast<unsigned char>(*s));
                        break;
                    }
                }
            }
        }

        // TCSR.TDV == 1 means TBUF still HOLDS a word (RM p.18-189), so "buffer free" is
        // TDV == 0; checking before the pop is what keeps a popped byte from having
        // nowhere to go.
        void drain_tx()
        {
            unsigned char b = 0;
            while (kickos::xmc::driver::usic_tx_ready(win_))
            {
                if (kos_byte_ring_pop_one(&sh_->tx, &b) == 0)
                {
                    return;
                }
                r32(win_ + ru::off::TBUF0) = b;
            }
        }

        // CCR is Write = PV: an unprivileged store inside a window this thread legitimately
        // holds is discarded by the bus with NO fault, so the read-back is the only
        // evidence either way.
        bool priv_write_verify(uintptr_t offset, uint32_t value)
        {
            if (kos_periph_reg_write(win_, offset, value) != 0)
            {
                return false;
            }
            return r32(win_ + offset) == value;
        }

        uintptr_t win_ = 0;
        kickos::uart::Shared* sh_ = nullptr;
    };

    // A console plain send carries no reply, so a short accept can be neither reported nor
    // retried by the sender: console_write_all carries the retry here instead, and its
    // bounded budget is what stops a dead channel wedging every stdout client forever.
    void console_write(kickos::uart::Shared* sh, unsigned char const* p, uint32_t n)
    {
        uint32_t const took = kickos::uart::console_write_all(sh, p, n);
        if (took >= n)
        {
            return;
        }
        sh->stats.tx_dropped += n - took;
    }

    void irq_thread(void* arg)
    {
        Ctx* ctx = static_cast<Ctx*>(arg);
        ConsoleUart dev;
        dev.init(ctx->win, &ctx->sh);
        if (not dev.bring_up())
        {
            // Panic, never kos_exit: once root has closed its own cap on the endpoint the
            // service thread is its sole receiver and would keep accepting stdout into a
            // ring nothing drains. The panic path reclaims the console (D6), so this
            // message reaches the wire.
            kos_panic("[xmcuartirq] U0C0 CCR privileged write refused or discarded");
        }
        kickos::uart::irq_loop(dev, &ctx->sh); // parks in irq_wait; never returns
    }

    // The console endpoint carries PLAIN SENDS of raw bytes (kos_console_publish routes
    // libc stdout to cap 0), which have no kos_uart_req framing. A kos_call on the same
    // endpoint arrives WITH a reply cap and is the wire ABI, so the recv must be
    // info-bearing to tell the two apart.
    void service_thread(void* arg)
    {
        Ctx* ctx = static_cast<Ctx*>(arg);
        unsigned char msg[KOS_EP_MSG_MAX];
        while (true)
        {
            struct kos_recv_info info;
            long const n = kos_recv(kickos::uart::KOS_UART_CAP_EP, msg, sizeof(msg), &info);
            if (n < 0)
            {
                break; // endpoint dead (EPIPE) or a bad cap: let the bring-up respawn us
            }
            if (info.reply_cap >= 0)
            {
                kickos::uart::serve_one(&ctx->sh, msg, static_cast<size_t>(n), info.reply_cap);
                continue;
            }
            if (n == 0)
            {
                (void)kickos::uart::console_flush(&ctx->sh); // zero-length plain send == flush
                continue;
            }
            console_write(&ctx->sh, msg, static_cast<uint32_t>(n));
        }
        kos_exit(0);
    }
}

extern "C"
{

int xmcuartirq_console_start(struct kos_service_cfg const* cfg)
{
    if (cfg == nullptr or cfg->kind != KOS_SVC_CONSOLE)
    {
        kos::print("[xmcuartirq] ERROR: bad or non-console service cfg\n");
        return -1;
    }

    // 1. The shared block: ONE power-of-two, naturally-aligned allocation, because the
    //    RAM arm of grant_region_admissible demands it of every caller including this
    //    one. kos_ram_alloc reserves but grants nothing, so reach it before writing it.
    void* blk = kos_ram_alloc(kickos::uart::KOS_UART_BLOCK_SIZE);
    if (blk == nullptr)
    {
        kos::print("[xmcuartirq] ERROR: arena cannot spare the ring block\n");
        return -1;
    }
    if (kos_mem_self_grant(blk, kickos::uart::KOS_UART_BLOCK_SIZE) != 0)
    {
        kos::print("[xmcuartirq] ERROR: mem_self_grant of the ring block refused\n");
        return -1;
    }
    Ctx* ctx = static_cast<Ctx*>(blk);
    kickos::uart::shared_init(&ctx->sh);
    ctx->win = cfg->mmio_base;

    // 2. The console endpoint.
    int const ep = kos_endpoint_create();
    if (ep < 0)
    {
        kos::print("[xmcuartirq] ERROR: endpoint_create failed\n");
        return -1;
    }

    // 3. Relinquish the kernel UART and route stdout to E. This MUST precede the claim
    //    below: console_tx_deinit stops TBIEN and irq_detach()es NVIC 84, so the line is
    //    only free for kos_irq_claim once the publish has returned.
    if (kos_console_publish(ep) != 0)
    {
        kos::print("[xmcuartirq] ERROR: console_publish failed\n");
        kos_handle_close(ep);
        return -1;
    }

    // From here the kernel console path is dark, so every failure closes E FIRST: that
    // takes the endpoint's last receiver holder to zero, which reclaims the console and
    // lets the tag actually reach the wire.

    // 4. The line. Claimed HERE because minting needs KOS_AUTH_IRQ and both driver
    //    threads run at authority 0. It comes back MASKED: the IRQ thread's first wait
    //    arms it, in the thread that will consume the event. EDGE, with no peripheral-side
    //    clear to pair with it: PSR.TBIF has no influence on interrupt generation and does
    //    not need clearing (RM 18.2.2.3 p.18-17), so PSCR.CTBIF is cosmetic here.
    int const irq = kos_irq_claim(kickos::xmc::irq::USIC0_SR0, KOS_IRQ_EDGE);
    if (irq < 0)
    {
        kos_handle_close(ep);
        kos::print("[xmcuartirq] ERROR: irq_claim(USIC0 SR0) failed\n");
        return -1;
    }

    // 5. The IRQ thread: the U0C0 window (R|W|DEV), the shared block, and the line
    //    (WAIT). Strictly ABOVE the service thread. The SERVICE thread gets no window at
    //    all: a DEV window has exactly one holder, so a second spawn asking for it is
    //    refused -KOS_EBUSY.
    //
    //    ORDER IS LOAD-BEARING: spawning the IRQ thread first is what makes the TX ring
    //    provably empty when bring_up() arms TBIEN. The line's FIRST irq_wait DISCARDS any
    //    pend latched before it (rearm_locked clears pending while not armed_once), so a
    //    byte that bring_up's drain managed to push would lose its transmit-buffer event
    //    and stall until the next doorbell.
    kos_cap_grant const irq_caps[1] = { { irq, KOS_CAP_WAIT } };
    int const irqt = kos::thread::spawn(irq_thread, ctx, "uartirq",
                                        static_cast<uint8_t>(cfg->prio + 1),
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                        /*mem=*/ctx, kickos::uart::KOS_UART_BLOCK_SIZE,
                                        /*stack=*/nullptr, /*stack_size=*/0,
                                        /*mmio=*/reinterpret_cast<void*>(cfg->mmio_base),
                                        cfg->mmio_window, irq_caps, 1);
    if (irqt < 0)
    {
        kos_handle_close(irq);
        kos_handle_close(ep);
        kos::print("[xmcuartirq] ERROR: IRQ thread spawn failed\n");
        return -1;
    }

    // 6. Wait for the IRQ thread's own device bring-up, STRICTLY BEFORE THE SERVICE THREAD
    //    EXISTS: root is still the endpoint's ONLY receiver holder here, so the close below
    //    takes recv_holders to zero and reclaims the console, which is what makes this
    //    timeout reportable. The handover probe proves only that the SERVICE thread is
    //    serving. Sleeping rather than spinning because the IRQ thread may sit below root's
    //    priority.
    //
    //    Every failure path from here on also CANCELS the IRQ thread, and closes BEFORE
    //    cancelling so the note is already set when the cancelled thread's exit runs the
    //    reclaim: the console is not given back while a live thread holds the register
    //    window (kernel/init/console.cc). Cancellation is cooperative, so the one case it
    //    cannot rescue is this very timeout with the IRQ thread wedged BEFORE its first
    //    kos_irq_wait: it is marked, does not die, and the tag is dropped.
    uint32_t waited = 0;
    while (ctx->sh.ready == 0u)
    {
        if (waited >= READY_WAIT_MAX)
        {
            kos_handle_close(irq);
            kos_handle_close(ep);
            (void)kos_thread_kill(irqt);
            kos::print("[xmcuartirq] ERROR: IRQ thread never reached its loop\n");
            return -1;
        }
        waited++;
        kos_sleep_ns(READY_WAIT_NS);
    }

    // 7. The service thread: the endpoint (WAIT) at child index 1 and the SAME line as
    //    the DOORBELL (SIGNAL only) at index 2. SIGNAL is a pure post on the binding, not
    //    a raise at the controller, so this thread starts a transfer without touching a
    //    register it does not own.
    kos_cap_grant const svc_caps[2] = { { ep, KOS_CAP_WAIT }, { irq, KOS_CAP_SIGNAL } };
    int const svct = kos::thread::spawn(service_thread, ctx, cfg->name, cfg->prio,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                        /*mem=*/ctx, kickos::uart::KOS_UART_BLOCK_SIZE,
                                        /*stack=*/nullptr, /*stack_size=*/0,
                                        /*mmio=*/nullptr, 0, svc_caps, 2);
    if (svct < 0)
    {
        kos_handle_close(irq);
        kos_handle_close(ep);
        (void)kos_thread_kill(irqt); // frees the window, which is what gives the console back
        kos::print("[xmcuartirq] ERROR: service thread spawn failed\n");
        return -1;
    }

    // Root's own line cap goes: with the two driver threads the only holders, the line
    // returns to the pool when BOTH die.
    kos_handle_close(irq);

    // 8. Close root's OWN WAIT-bearing cap on E (S4), then PROVE the driver is serving
    //    before returning.
    return kickos::driver::console_handover_finish(
        ep, "[xmcuartirq] ERROR: driver died during bring-up\n", irqt);
}

}
