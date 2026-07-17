// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/DSPI0 blocking SPI transport (see spi_transport.h). A privileged bring-up
// shim configures DSPI0 + opens its AIPS slot + spawns the UNPRIVILEGED driver
// thread; client threads call spi_transfer()/spi_enable_cs()/spi_disable_cs(), which
// hand a descriptor to the driver over a semaphore handshake and block. The driver
// runs EOQ-terminated DSPI frames (up to the 4-entry TX FIFO per batch, one EOQ wake
// per batch) and W1Cs SR.EOQF before the next kos_irq_wait re-arms line 26 (else the
// level re-asserts on unmask and storms -- the k64drv/PIT-TIF hazard).
//
// K64F peripheral privilege is gated by the AIPS bridge (PACR), NOT SYSMPU: clearing
// DSPI0 slot-44 PACR SP admits EVERY unprivileged thread; the SYSMPU MMIO grant is
// inert for the peripheral (documented, unchanged from the loopback demo). There is
// no per-thread peripheral boundary on K64F -- the AIPS ceiling.
//
// Cross-thread buffers: the driver thread and a client thread are different domains,
// so the driver CANNOT read a client's private stack. spi_transfer() therefore copies
// the caller's tx into a shared bounce buffer (client context) and copies rx back out
// after completion. The bounce buffer + descriptor live in the app's .appdata window,
// granted R|W to every unprivileged app thread by the kernel.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include "spi_transport.h"

#include <stdint.h>

namespace
{
    // --- DSPI0 / SIM / PORT / AIPS register map (K64 RM ch.50, 12.2, 11.5, 20.2) ---
    constexpr uintptr_t SIM_SCGC5 = 0x40048038u; // RM 12.2.12: PORTx clock gates
    constexpr uintptr_t SIM_SCGC6 = 0x4004803Cu; // RM 12.2.13
    constexpr uint32_t SCGC5_PORTD = 1u << 12;
    constexpr uint32_t SCGC6_SPI0 = 1u << 12;

    constexpr uint32_t SCGC5_PORTC = 1u << 11;

    constexpr uintptr_t PORTD_BASE = 0x4004C000u;
    constexpr uintptr_t PORTD_PCR0 = PORTD_BASE + 0x00u;
    constexpr uintptr_t PORTD_PCR1 = PORTD_BASE + 0x04u;
    constexpr uintptr_t PORTD_PCR2 = PORTD_BASE + 0x08u;
    constexpr uintptr_t PORTD_PCR3 = PORTD_BASE + 0x0Cu;
    constexpr uint32_t PCR_MUX_ALT2 = 0x2u << 8; // PTD1/2/3 -> SCK/SOUT/SIN; PCS0 pad below

    // EasyCAT LAN9252 shield: SCS is on Arduino D9 = PTC4, NOT D10/PTD0. PTC4/ALT2 is
    // ALSO SPI0_PCS0 (K64 RM signal-mux, PTC4 row), so hardware PCS0 routes to it --
    // no GPIO/software CS; the PUSHR.PCS0/CONT path is unchanged.
    constexpr uintptr_t PORTC_BASE = 0x4004B000u;
    constexpr uintptr_t PORTC_PCR4 = PORTC_BASE + 0x10u;

    constexpr uintptr_t SPI0_BASE = 0x4002C000u; // AIPS0 slot 44
    constexpr uint32_t MCR_OFFSET = 0x00u;
    constexpr uint32_t CTAR0_OFFSET = 0x0Cu;
    constexpr uint32_t SR_OFFSET = 0x2Cu;
    constexpr uint32_t RSER_OFFSET = 0x30u;
    constexpr uint32_t PUSHR_OFFSET = 0x34u;
    constexpr uint32_t POPR_OFFSET = 0x38u;

    // 64 B window covers MCR(0x00)..POPR(0x38): SYSMPU-encodable (32-aligned, 32-mult)
    // and pow2+64-aligned so the identical grant encodes on PMSA/PMP too.
    constexpr uint32_t SPI0_WINDOW = 0x40u;

    // MCR (RM 50.3.1)
    constexpr uint32_t MCR_MSTR = 1u << 31;
    constexpr uint32_t MCR_PCSIS0 = 1u << 16; // PCS0 inactive-high (ESC CS idle high)
    constexpr uint32_t MCR_CLR_TXF = 1u << 11;
    constexpr uint32_t MCR_CLR_RXF = 1u << 10;
    constexpr uint32_t MCR_HALT = 1u << 0;

    // CTAR0 (RM 50.3.2): FMSZ=7 => 8-bit; CPOL=0/CPHA=0 => SPI mode 0.
    constexpr uint32_t CTAR0_FMSZ_8BIT = 7u << 27;
    // Conservative loopback baud (PBR /7, BR scaler 16) -- fine over a jumper.
    constexpr uint32_t CTAR0_PBR_DIV7 = 0x3u << 16;
    constexpr uint32_t CTAR0_BR_SC16 = 0x4u << 0;
    // Conservative real-ESC bring-up baud: PBR /2, BR scaler 16 -> ~1.9 MHz at a 60 MHz
    // bus, well within the LAN9252 SPI ceiling and a safe first-contact rate. Raise once
    // BYTE_TEST is proven; the datasheet CS/inter-frame delays (CSSCK/ASC/DT) should also
    // be set here for higher rates (bench-verify).
    constexpr uint32_t CTAR0_PBR_DIV2 = 0x0u << 16;

    // SR (RM 50.3.5) / RSER (RM 50.3.6)
    constexpr uint32_t SR_EOQF = 1u << 28; // w1c
    constexpr uint32_t RSER_EOQF_RE = 1u << 28;

    // PUSHR (RM 50.3.7, master): CONT b31, EOQ b27, PCS b16, TXDATA [15:0]. CTAS=0.
    constexpr uint32_t PUSHR_CONT = 1u << 31;
    constexpr uint32_t PUSHR_EOQ = 1u << 27;
    constexpr uint32_t PUSHR_PCS0 = 1u << 16;

    constexpr int SPI0_IRQ = 26; // RM ch.3 vector table: DSPI0 single vector

    constexpr uint32_t TX_FIFO_DEPTH = 4u; // SPI0 TX FIFO = 4 entries

    // Largest single logical transfer. LAN9252 CSR accesses are <= 7 bytes
    // (3-byte cmd + <=4 aligned payload); process-data bursts stay well under this.
    constexpr size_t BOUNCE_MAX = 256u;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // --- Shared transport state (lands in the app .appdata window: R|W to every
    // unprivileged app thread, readable by the privileged shim). Semaphores order
    // the single-outstanding request; g_lock serializes concurrent clients. ---
    int g_lock = -1; // mutex (init 1): one transfer at a time
    int g_req = -1;  // driver waits (init 0): client posts a ready descriptor
    int g_done = -1; // client waits (init 0): driver posts completion

    unsigned char g_bounce[BOUNCE_MAX]; // tx in / rx out (full-duplex, in place)
    volatile size_t g_len = 0;          // frame count for this request
    volatile int g_cont = 0;            // hold CS after the transfer (PUSHR.CONT)
    volatile int g_release = 0;         // request is a bare CS-release, no data
    volatile int g_result = 0;          // driver -> client: bytes, or <0

    int g_cs_hold = 0; // client-side CS state, sampled into g_cont per transfer

    void mem_copy(void* dst, void const* src, size_t n)
    {
        unsigned char* d = static_cast<unsigned char*>(dst);
        unsigned char const* s = static_cast<unsigned char const*>(src);
        for (size_t i = 0; i < n; i++)
        {
            d[i] = s[i];
        }
    }

    void mem_zero(void* dst, size_t n)
    {
        unsigned char* d = static_cast<unsigned char*>(dst);
        for (size_t i = 0; i < n; i++)
        {
            d[i] = 0;
        }
    }

    // Run the descriptor now sitting in the shared state on the DSPI window. Batches
    // frames up to the TX FIFO depth; EOQ terminates each batch (one wake per batch);
    // POPR drains the RX FIFO into g_bounce in place; SR.EOQF is W1C'd before the next
    // kos_irq_wait re-arms line 26.
    void run_transfer(uintptr_t win, int irq)
    {
        volatile uint32_t* sr = reinterpret_cast<volatile uint32_t*>(win + SR_OFFSET);
        volatile uint32_t* pushr = reinterpret_cast<volatile uint32_t*>(win + PUSHR_OFFSET);
        volatile uint32_t* popr = reinterpret_cast<volatile uint32_t*>(win + POPR_OFFSET);

        if (g_release != 0)
        {
            // Bare CS-release: negate PCS0 with one CONT=0 frame. NOTE (bench-verify,
            // the K64F HW-PCS caveat): DSPI has no zero-clock CS deassert, so this
            // clocks one dummy 0x00. Harmless for a preceding LAN9252 READ (the extra
            // byte is discarded), but a preceding WRITE would see a trailing 0x00 --
            // Stage D must confirm on silicon whether K64 DSPI auto-negates PCS at
            // EOQ/STOPPED (making this a no-op) or coalesce cmd+payload for writes.
            *pushr = PUSHR_EOQ | PUSHR_PCS0 | 0x00u;
            kos_irq_wait(irq);
            (void)*popr;
            *sr = SR_EOQF;
            g_result = 0;
            return;
        }

        size_t const len = g_len;
        int const cont = g_cont;
        size_t i = 0;
        while (i < len)
        {
            size_t batch = len - i;
            if (batch > TX_FIFO_DEPTH)
            {
                batch = TX_FIFO_DEPTH;
            }

            for (size_t j = 0; j < batch; j++)
            {
                size_t const idx = i + j;
                uint32_t frame = PUSHR_PCS0 | (g_bounce[idx] & 0xFFu);

                // CONT holds PCS0 between frames. Every frame keeps CS asserted except
                // the transfer's final frame, which follows the caller's CS-hold: held
                // (CONT=1) keeps CS for the next transfer, not held (CONT=0) releases.
                bool const last_overall = (idx == len - 1);
                if (not last_overall or cont != 0)
                {
                    frame |= PUSHR_CONT;
                }

                // EOQ terminates each FIFO batch: one wake per batch (== one wake per
                // logical transfer for the len<=4 CSR accesses). CONT held across the
                // EOQ/STOPPED boundary keeps CS asserted between batches.
                if (j == batch - 1)
                {
                    frame |= PUSHR_EOQ;
                }

                *pushr = frame;
            }

            kos_irq_wait(irq); // block until this batch's EOQF raises line 26

            for (size_t j = 0; j < batch; j++)
            {
                g_bounce[i + j] = static_cast<unsigned char>(*popr & 0xFFu);
            }

            *sr = SR_EOQF; // W1C before the next kos_irq_wait re-arms (anti-storm)
            i += batch;
        }

        g_result = static_cast<int>(len);
    }

    // UNPRIVILEGED driver thread: owns the DSPI0 window (spawn MMIO grant) + IRQ 26
    // (tier-1). The window base arrives as the arg VALUE (never dereferenced as
    // memory); the shared descriptor lives in the granted .appdata window.
    void dspi_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        int h = kos_irq_register(SPI0_IRQ);
        if (h < 0)
        {
            kos::print("[k64dspi] ERROR: irq_register(DSPI0) failed\n");
            while (true)
            {
                kos_sleep_ns(1000000000ull);
            }
        }

        // Announce before the first blocking wait: if IRQ 26 never fires (misrouted
        // line / NVIC / RSER) the driver hangs in kos_irq_wait -- this disambiguates a
        // hung-waiting-for-IRQ board from a dead one / missing console.
        kos::print("[k64dspi] transport ready (DSPI0 driver waiting for requests)\n");

        while (true)
        {
            kos_sem_wait(g_req); // block until a client posts a descriptor
            run_transfer(win, h);
            kos_sem_post(g_done);
        }
    }
}

extern "C"
{
    int spi_driver_start(int loopback)
    {
        // Handshake semaphores FIRST: a client may call spi_transfer() before the
        // driver thread is scheduled; the counting g_req buffers that post.
        g_lock = kos_sem_create(1);
        g_req = kos_sem_create(0);
        g_done = kos_sem_create(0);
        if (g_lock < 0 or g_req < 0 or g_done < 0)
        {
            kos::print("[k64dspi] ERROR: semaphore create failed\n");
            return -1;
        }

        // Privileged bring-up (this runs in the privileged app main): the one-time
        // unsafe setup the unprivileged driver must NOT do -- clock gates + PORTD
        // pin-mux + DSPI master config + AIPS slot open. SIM (could ungate any
        // peripheral) and PORTD (could re-mux SPI onto arbitrary pins) stay
        // privileged; the driver gets ONLY the DSPI window + the DSPI IRQ.
        r32(SIM_SCGC5) |= SCGC5_PORTD;
        r32(SIM_SCGC6) |= SCGC6_SPI0;

        // Mux SCK/SOUT/SIN on PTD1/PTD2/PTD3 (Arduino D13/D11/D12) -> ALT2. Glitch-free
        // before DSPI config only because CPOL=0 (mode 0) idle matches the pin idle at
        // mux time; a CPOL=1 ESC MUST program CTAR before muxing (the "mux last" point).
        r32(PORTD_PCR1) = PCR_MUX_ALT2; // SCK  (D13)
        r32(PORTD_PCR2) = PCR_MUX_ALT2; // SOUT (D11)
        r32(PORTD_PCR3) = PCR_MUX_ALT2; // SIN  (D12)

        // Hardware PCS0 pad follows the wiring: loopback bench uses the DSPI-native
        // PTD0 (Arduino D10); the EasyCAT LAN9252 shield wires SCS to D9 = PTC4.
        if (loopback != 0)
        {
            r32(PORTD_PCR0) = PCR_MUX_ALT2; // PCS0 (D10)
        }
        else
        {
            r32(SIM_SCGC5) |= SCGC5_PORTC;
            r32(PORTC_PCR4) = PCR_MUX_ALT2; // PCS0 (D9)
        }

        // DSPI0 config while HALTed. MCR resets 0x0000_4001 (MDIS=1, HALT=1): this
        // write clears MDIS, flushes both FIFOs, sets master + PCS0-idle-high, holds
        // HALT during config.
        r32(SPI0_BASE + MCR_OFFSET) =
            MCR_MSTR | MCR_PCSIS0 | MCR_CLR_TXF | MCR_CLR_RXF | MCR_HALT;

        uint32_t ctar = CTAR0_FMSZ_8BIT;
        if (loopback != 0)
        {
            ctar |= CTAR0_PBR_DIV7 | CTAR0_BR_SC16;
        }
        else
        {
            ctar |= CTAR0_PBR_DIV2 | CTAR0_BR_SC16;
        }
        r32(SPI0_BASE + CTAR0_OFFSET) = ctar;

        r32(SPI0_BASE + RSER_OFFSET) = RSER_EOQF_RE; // route EOQ flag to NVIC IRQ 26

        // Open DSPI0 slot 44 to user mode: clear PACR44 SP (bit 14 of AIPS0_PACRF,
        // field [15:12]). RM 20.2 -- the ACTUAL enabler on K64F; the SYSMPU grant
        // below is inert for the peripheral. PACRF resets SP=1 (supervisor-only).
        constexpr uintptr_t AIPS0_PACRF = 0x40000044u;
        constexpr uint32_t PACR44_SP = 1u << 14;
        r32(AIPS0_PACRF) &= ~PACR44_SP;

        // Release HALT -> RUNNING; the first PUSHR starts the queue.
        r32(SPI0_BASE + MCR_OFFSET) = MCR_MSTR | MCR_PCSIS0;

        int drv = kos::thread::spawn(dspi_driver, reinterpret_cast<void*>(SPI0_BASE),
                                     "k64dspi", 10, KOS_POLICY_FIFO, 0,
                                     /*privileged=*/false,
                                     /*mem=*/nullptr, /*mem_size=*/0,
                                     /*stack=*/nullptr, /*stack_size=*/0,
                                     /*mmio=*/reinterpret_cast<void*>(SPI0_BASE),
                                     SPI0_WINDOW);
        if (drv < 0)
        {
            kos::print("[k64dspi] ERROR: driver spawn failed\n");
            return -1;
        }
        return 0;
    }

    int spi_transfer(void* tx, void* rx, size_t len)
    {
        if (len == 0)
        {
            return 0;
        }
        if (len > BOUNCE_MAX)
        {
            return -1;
        }
        if (g_lock < 0)
        {
            return -1; // transport not started
        }

        kos_sem_wait(g_lock);

        if (tx != nullptr)
        {
            mem_copy(g_bounce, tx, len);
        }
        else
        {
            mem_zero(g_bounce, len); // null tx -> shift dummy 0x00
        }
        g_len = len;
        g_cont = g_cs_hold;
        g_release = 0;

        kos_sem_post(g_req);
        kos_sem_wait(g_done);

        int r = g_result;
        if (rx != nullptr and r >= 0)
        {
            mem_copy(rx, g_bounce, len); // null rx -> discard
        }

        kos_sem_post(g_lock);
        return r;
    }

    void spi_enable_cs(void)
    {
        g_cs_hold = 1;
    }

    void spi_disable_cs(void)
    {
        g_cs_hold = 0;
        if (g_lock < 0)
        {
            return;
        }
        // Hand the driver a bare CS-release (see run_transfer for the K64F caveat).
        kos_sem_wait(g_lock);
        g_len = 0;
        g_release = 1;
        kos_sem_post(g_req);
        kos_sem_wait(g_done);
        kos_sem_post(g_lock);
    }
}
