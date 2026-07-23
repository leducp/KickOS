// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/DSPI0 blocking SPI transport (see <kickos/driver/k64dspi.h>). A privileged bring-up
// shim configures DSPI0 + opens its AIPS slot + spawns the UNPRIVILEGED driver
// thread; client threads call spi_transfer()/spi_enable_cs()/spi_disable_cs(), which
// hand a descriptor to the driver over a semaphore handshake and block. The driver
// runs EOQ-terminated DSPI frames (up to the 4-entry TX FIFO per batch, one EOQ wake
// per batch) and W1Cs SR.EOQF before the next kos_irq_wait re-arms line 26 (else the
// level re-asserts on unmask and storms -- the k64drv/PIT-TIF hazard).
//
// Chip select is a SOFTWARE GPIO on PTC4 (Arduino D9 = LAN9252 SCS), NOT hardware
// PCS0. DSPI clocks the data frames; the driver thread brackets them by driving the
// PTC4 GPIO low (assert) / high (release). This mirrors the NuttX kinetis SPI driver
// and fixes the confirmed Stage-D bug: DSPI's CONT/PCS model has no zero-clock CS
// deassert, so releasing hardware PCS0 clocked a trailing dummy 0x00. Fixed-size CSR
// writes tolerated it (INIT->PRE_OP worked) but the length-sensitive mailbox/process-
// RAM FIFO write (the SDO response) was corrupted -> master discards -> SDO timeout.
//
// GPIO access path (K64 RM 3.10.1.1 / 3.3.6.2 / 3.3.7.1 / 4.6): the GPIO module has
// NO access protection -- it is a direct crossbar slave (port 3), not an AIPS-Lite
// slot (so it has no PACR) and NOT an MPU slave port (MPU covers flash/SRAM/FlexBus
// only). So the unprivileged driver reaches GPIOC's data registers with NO grant:
// no PACR to open for GPIO, and the SYSMPU MMIO grant is inert (GPIO bypasses the
// MPU entirely). Only the PTC4 pin-mux (PORTC PCR4, an AIPS peripheral) + direction
// are set PRIVILEGED one-time in spi_driver_start; the per-transaction PSOR/PCOR
// toggles run in the unprivileged driver thread. DSPI0 privilege stays AIPS-PACR
// slot 44 as before.
//
// Cross-thread buffers: the driver thread and a client thread are different domains,
// so the driver CANNOT read a client's private stack. spi_transfer() therefore copies
// the caller's tx into a shared bounce buffer (client context) and copies rx back out
// after completion. The bounce buffer + descriptor live in the app's .appdata window,
// granted R|W to every unprivileged app thread by the kernel.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <kickos/driver/k64dspi.h>

#include <dspi_class.h> // Rule 6 class-driver leaf: shared DSPI RX-FIFO fill-level read

#include <stdint.h>

namespace
{
    // --- DSPI0 / SIM / PORT / GPIO register map (K64 RM ch.50, 12.2, 11.5, 55.2) ---
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
    constexpr uint32_t PCR_MUX_ALT1 = 0x1u << 8; // RM 11.5 PORTx_PCRn MUX=001 -> GPIO
    constexpr uint32_t PCR_MUX_ALT2 = 0x2u << 8; // PTD1/2/3 -> SCK/SOUT/SIN

    // LAN9252 shield: SCS is on Arduino D9 = PTC4. Software GPIO CS drives this pin
    // (PTC4/ALT1 = GPIO), NOT hardware SPI0_PCS0 (PTC4/ALT2). The PCR4 mux + direction
    // are set privileged in spi_driver_start; per-transaction toggles run in the driver.
    constexpr uintptr_t PORTC_BASE = 0x4004B000u;
    constexpr uintptr_t PORTC_PCR4 = PORTC_BASE + 0x10u;

    // GPIOC (K64 RM 55.2): direct crossbar slave at 0x400F_F080, system-clocked (RM
    // 55.1.1), NOT AIPS/MPU-gated (RM 3.10.1.1) -- unprivileged driver reaches it free.
    constexpr uintptr_t GPIOC_BASE = 0x400FF080u;
    constexpr uintptr_t GPIOC_PSOR = GPIOC_BASE + 0x04u; // set   -> PTC4 high (CS idle)
    constexpr uintptr_t GPIOC_PCOR = GPIOC_BASE + 0x08u; // clear -> PTC4 low  (CS asserted)
    constexpr uintptr_t GPIOC_PDDR = GPIOC_BASE + 0x14u; // 1 = output
    constexpr uint32_t CS_PIN = 1u << 4;                 // PTC4

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

    // MCR (RM 50.3.1). No PCSIS: hardware chip select is unused (GPIO CS instead).
    constexpr uint32_t MCR_MSTR = 1u << 31;
    constexpr uint32_t MCR_CLR_TXF = 1u << 11;
    constexpr uint32_t MCR_CLR_RXF = 1u << 10;
    constexpr uint32_t MCR_HALT = 1u << 0;

    // CTAR0 (RM 50.3.2): FMSZ=7 => 8-bit; CPOL=0/CPHA=0 => SPI mode 0.
    constexpr uint32_t CTAR0_FMSZ_8BIT = 7u << 27;
    // Conservative loopback baud (PBR /7, BR scaler 16) -- fine over a jumper.
    constexpr uint32_t CTAR0_PBR_DIV7 = 0x3u << 16;
    constexpr uint32_t CTAR0_BR_SC16 = 0x4u << 0;
    // Real-ESC baud ~10 MHz (PBR /3, BR scaler 2 -> 60/(3*2)), matching the NuttX
    // example. BYTE_TEST proved the link; the slow ~1.9 MHz rate (PBR /2, BR scaler 16)
    // plus the per-batch IRQ latency made the 512-byte mailbox read exceed the LAN9252's
    // 10 ms internal read timeout (-ETIMEDOUT), so the mailbox never processed.
    constexpr uint32_t CTAR0_PBR_DIV3 = 0x1u << 16;
    constexpr uint32_t CTAR0_BR_SC2 = 0x0u << 0;

    // SR (RM 50.3.5) / RSER (RM 50.3.6)
    constexpr uint32_t SR_EOQF = 1u << 28; // w1c
    constexpr uint32_t RSER_EOQF_RE = 1u << 28;

    // PUSHR (RM 50.3.7, master): EOQ b27, TXDATA [15:0], CTAS=0. No PCS/CONT -- the
    // GPIO CS frames the transaction, so no hardware chip select and no inter-frame
    // CS hold. EOQ still terminates each FIFO batch to raise the completion IRQ.
    constexpr uint32_t PUSHR_EOQ = 1u << 27;

    constexpr int SPI0_IRQ = 26; // RM ch.3 vector table: DSPI0 single vector

    constexpr uint32_t TX_FIFO_DEPTH = 4u; // SPI0 TX FIFO = 4 entries
    constexpr size_t RX_FIFO_DEPTH = 4u;   // SPI0 RX FIFO = 4 entries

    // Largest single logical transfer. LAN9252 CSR accesses are <= 7 bytes
    // (3-byte cmd + <=4 aligned payload); process-data bursts stay well under this.
    constexpr size_t BOUNCE_MAX = 256u;

    // Request kinds carried in the shared descriptor (client -> driver).
    constexpr int REQ_XFER = 0;    // clock g_len bytes through the DSPI FIFO
    constexpr int REQ_CS_LOW = 1;  // assert CS  (drive PTC4 low)
    constexpr int REQ_CS_HIGH = 2; // release CS (drive PTC4 high)

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
    volatile int g_req_kind = REQ_XFER; // REQ_XFER / REQ_CS_LOW / REQ_CS_HIGH
    volatile int g_result = 0;          // driver -> client: bytes, or <0

    // Driver-thread-only state (single writer: the driver). g_cs_active tracks an open
    // enable_cs..disable_cs bracket so a transfer inside it does NOT toggle CS; a bare
    // transfer (no bracket) self-brackets. g_gpio_cs gates the GPIO drive to the real
    // LAN9252 wiring (loopback bench has no CS and never gates PORTC).
    int g_cs_active = 0;
    int g_gpio_cs = 0;

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

    // GPIO software CS on PTC4 (driver-thread context). PSOR/PCOR are write-only atomic
    // set/clear -- no read-modify-write, no race with any other PTC bit. Both are Device
    // memory, kept in program order with the DSPI PUSHR stores on Cortex-M4, so CS-low
    // is observed before the first SCK and CS-high after the last frame's POPR drain.
    void cs_low(void)
    {
        if (g_gpio_cs != 0)
        {
            r32(GPIOC_PCOR) = CS_PIN;
        }
    }

    void cs_high(void)
    {
        if (g_gpio_cs != 0)
        {
            r32(GPIOC_PSOR) = CS_PIN;
        }
    }

    // Run the descriptor now sitting in the shared state. CS requests drive the GPIO;
    // a data request POLLS the DSPI FIFO (fill TX, drain RX, repeat) -- no EOQ, no
    // per-batch IRQ wait -- because the GPIO CS holds the line across the whole
    // transfer. One tight loop clocks N bytes (the batch+IRQ model spent ~1 reschedule
    // per 4 bytes, so a 512-byte mailbox read blew the LAN9252's 10 ms read window). CS
    // is asserted before the first clock and released after the last byte drains, but
    // only when the transfer is NOT inside an explicit enable_cs..disable_cs bracket
    // (g_cs_active), so CS is never toggled between the cmd and payload frames of one
    // CSR access.
    void run_request(uintptr_t win)
    {
        if (g_req_kind == REQ_CS_LOW)
        {
            cs_low();
            g_cs_active = 1;
            g_result = 0;
            return;
        }
        if (g_req_kind == REQ_CS_HIGH)
        {
            cs_high();
            g_cs_active = 0;
            g_result = 0;
            return;
        }

        volatile uint32_t* sr = reinterpret_cast<volatile uint32_t*>(win + SR_OFFSET);
        volatile uint32_t* pushr = reinterpret_cast<volatile uint32_t*>(win + PUSHR_OFFSET);
        volatile uint32_t* popr = reinterpret_cast<volatile uint32_t*>(win + POPR_OFFSET);

        size_t const len = g_len;

        bool const self_bracket = (g_cs_active == 0);
        if (self_bracket)
        {
            cs_low();
        }

        // Polled full-duplex. Fill the TX FIFO as far as it will go, drain whatever has
        // arrived, repeat -- SR TXCTR[15:12] = TX FIFO count, RXCTR[7:4] = RX count.
        // No EOQ, no per-batch IRQ: at 4-entry depth + ~10 MHz the FIFO-cycle time is
        // shorter than the reschedule latency, so spinning beats sleeping for the short
        // CSR/mailbox transfers here (a deep FIFO or eDMA would flip that -- see TODO).
        size_t pushed = 0;
        size_t popped = 0;
        while (popped < len)
        {
            // Drain first: the 4-deep RX FIFO must never overflow (a dropped byte would
            // leave popped < len forever -> hang). RX fill level via the shared leaf.
            if (kickos::mk64f::classdrv::dspi_rx_count(win) > 0u)
            {
                g_bounce[popped] = static_cast<unsigned char>(*popr & 0xFFu);
                popped++;
            }
            // Push only while fewer than RX_FIFO_DEPTH bytes are IN FLIGHT, so every
            // completed frame always has a free RX slot. Capping on TXCTR alone is not
            // enough: in-flight = TX FIFO + shift register + RX FIFO, so filling TX to
            // its own depth can push RX past 4 and drop a byte (the 512-byte read hang).
            if (pushed < len and (pushed - popped) < RX_FIFO_DEPTH
                and ((*sr >> 12) & 0xFu) < TX_FIFO_DEPTH)
            {
                *pushr = static_cast<uint32_t>(g_bounce[pushed]) & 0xFFu;
                pushed++;
            }
        }

        if (self_bracket)
        {
            cs_high();
        }

        g_result = static_cast<int>(len);
    }

    // UNPRIVILEGED driver thread: owns the DSPI0 window (spawn MMIO grant) + IRQ 26
    // (tier-1) and drives the PTC4 GPIO CS (ungated by MPU/AIPS -- RM 3.10.1.1). The
    // window base arrives as the arg VALUE (never dereferenced as memory); the shared
    // descriptor lives in the granted .appdata window.
    void dspi_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        kos::print("[k64dspi] transport ready (DSPI0 driver, polled FIFO)\n");

        while (true)
        {
            kos_sem_wait(g_req); // block until a client posts a descriptor
            run_request(win);
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
        // unsafe setup the unprivileged driver must NOT do -- clock gates + PORT
        // pin-mux + GPIO CS direction + DSPI master config + AIPS slot open. SIM (could
        // ungate any peripheral) and PORT (could re-mux SPI onto arbitrary pins) stay
        // privileged; the driver gets ONLY the DSPI window + the DSPI IRQ + the free
        // (un-gated) GPIOC data registers it toggles per transaction.
        r32(SIM_SCGC5) |= SCGC5_PORTD;
        r32(SIM_SCGC6) |= SCGC6_SPI0;

        // Mux SCK/SOUT/SIN on PTD1/PTD2/PTD3 (Arduino D13/D11/D12) -> ALT2. Glitch-free
        // before DSPI config only because CPOL=0 (mode 0) idle matches the pin idle at
        // mux time; a CPOL=1 ESC MUST program CTAR before muxing (the "mux last" point).
        r32(PORTD_PCR1) = PCR_MUX_ALT2; // SCK  (D13)
        r32(PORTD_PCR2) = PCR_MUX_ALT2; // SOUT (D11)
        r32(PORTD_PCR3) = PCR_MUX_ALT2; // SIN  (D12)

        // Chip select. Loopback bench keeps the DSPI-native hardware PCS0 on PTD0 (D10)
        // -- inert now (frames carry no PCS) but harmless for the SOUT->SIN self-test.
        // The LAN9252 shield uses a SOFTWARE GPIO CS on PTC4 (D9): preload PDOR high,
        // set the pin to output, THEN mux ALT1 (GPIO) so the pin drives high the instant
        // it becomes an output -- CS idle high, no assert glitch at bring-up.
        if (loopback != 0)
        {
            r32(PORTD_PCR0) = PCR_MUX_ALT2; // hardware PCS0 (D10), unused by the frames
        }
        else
        {
            r32(SIM_SCGC5) |= SCGC5_PORTC;  // PORTC PCR clock (GPIO data is system-clocked)
            r32(GPIOC_PSOR) = CS_PIN;       // latch PDOR high (CS idle)
            r32(GPIOC_PDDR) |= CS_PIN;      // PTC4 -> output
            r32(PORTC_PCR4) = PCR_MUX_ALT1; // PTC4 -> GPIO (was ALT2 = SPI0_PCS0)
            g_gpio_cs = 1;
        }

        // DSPI0 config while HALTed. MCR resets 0x0000_4001 (MDIS=1, HALT=1): this
        // write clears MDIS, flushes both FIFOs, sets master, holds HALT during config.
        r32(SPI0_BASE + MCR_OFFSET) =
            MCR_MSTR | MCR_CLR_TXF | MCR_CLR_RXF | MCR_HALT;

        uint32_t ctar = CTAR0_FMSZ_8BIT;
        if (loopback != 0)
        {
            ctar |= CTAR0_PBR_DIV7 | CTAR0_BR_SC16;
        }
        else
        {
            ctar |= CTAR0_PBR_DIV3 | CTAR0_BR_SC2;
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
        r32(SPI0_BASE + MCR_OFFSET) = MCR_MSTR;

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
        g_req_kind = REQ_XFER;

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

    // Assert CS (drive PTC4 low) and hold it across the following transfer pair. The
    // driver performs the GPIO write, ordered before the next transfer's frames by the
    // g_req/g_done handshake. Single SPI client assumed: g_lock serializes individual
    // ops, not the whole enable..disable bracket (unchanged from the hardware-PCS path).
    void spi_enable_cs(void)
    {
        if (g_lock < 0)
        {
            return;
        }
        kos_sem_wait(g_lock);
        g_req_kind = REQ_CS_LOW;
        kos_sem_post(g_req);
        kos_sem_wait(g_done);
        kos_sem_post(g_lock);
    }

    // Release CS (drive PTC4 high) after the transfer pair. The driver has already
    // drained the last frame (POPR read) before this GPIO write, so CS rises strictly
    // after the final clock -- no trailing dummy byte (the Stage-D bug fix).
    void spi_disable_cs(void)
    {
        if (g_lock < 0)
        {
            return;
        }
        kos_sem_wait(g_lock);
        g_req_kind = REQ_CS_HIGH;
        kos_sem_post(g_req);
        kos_sem_wait(g_done);
        kos_sem_post(g_lock);
    }
}
