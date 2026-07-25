// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/DSPI0 SPI bus SERVICE (see <kickos/driver/k64dspi.h>). Three parts:
//
//   1. DspiBus -- the transaction ENGINE (a class, no globals, no IPC). Given the
//      granted DSPI0 window base it derives a CTAR from a target Hz/mode/word, and
//      clocks N bytes full-duplex through the polled 4-deep FIFO with the PTC4 GPIO
//      CS bracketing the WHOLE transfer (assert before the first clock, release
//      after the last RX drain -- the Stage-D coherent-transaction fix).
//   2. spi_service -- the unprivileged driver thread: kos_recv a kos_bus_req,
//      validate it, run the class transaction over the concatenated segment bytes,
//      kos_reply a kos_bus_rsp. The reply cap is consumed on EVERY loop path.
//   3. k64dspi_spi_start -- the privileged one-time bring-up + endpoint + spawn.
//
// Chip select is a SOFTWARE GPIO on PTC4, NOT hardware PCS0: DSPI's CONT/PCS model
// has no zero-clock CS deassert, so releasing hardware PCS0 clocked a trailing dummy
// byte that corrupted length-sensitive LAN9252 mailbox writes (the Stage-D bug). The
// GPIO write path is ungated (K64 RM 3.10.1.1: GPIO is a direct crossbar slave with
// no PACR and no SYSMPU coverage), so the unprivileged driver toggles PSOR/PCOR with
// no grant; only the PTC4 pin-mux + direction are set privileged in the bring-up.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/k64dspi.h>

#include <kickos/sys/bus.h>         // kos_bus_req/seg/rsp/cfg wire ABI
#include <kickos/sys/service.h>     // kos_service_cfg (base/window/prio/cs as data)
#include <kickos/sys/bytes.h>          // mem_copy
#include <kickos/sys/spi_service.h>    // kickos::spi::serve_loop (shared choreography)
#include <kickos/sys/driver_bringup.h> // kickos::driver::spawn_unprivileged
#include <kickos/io/mmio.h>            // r32

#include <dspi_class.h> // Rule 6 class-driver leaf: shared DSPI RX-FIFO fill-level read

#include <stdint.h>
#include <stddef.h>

namespace
{
    // --- DSPI0 / SIM / PORT / GPIO register map (K64 RM ch.50, 12.2, 11.5, 55.2) ---
    constexpr uintptr_t SIM_SCGC5 = 0x40048038u; // RM 12.2.12: PORTx clock gates
    constexpr uintptr_t SIM_SCGC6 = 0x4004803Cu; // RM 12.2.13
    constexpr uint32_t SCGC5_PORTD = 1u << 12;
    constexpr uint32_t SCGC6_SPI0 = 1u << 12;
    constexpr uint32_t SCGC5_PORTC = 1u << 11;

    constexpr uintptr_t PORTD_BASE = 0x4004C000u;
    constexpr uintptr_t PORTD_PCR1 = PORTD_BASE + 0x04u;
    constexpr uintptr_t PORTD_PCR2 = PORTD_BASE + 0x08u;
    constexpr uintptr_t PORTD_PCR3 = PORTD_BASE + 0x0Cu;
    constexpr uint32_t PCR_MUX_ALT1 = 0x1u << 8; // RM 11.5 PORTx_PCRn MUX=001 -> GPIO
    constexpr uint32_t PCR_MUX_ALT2 = 0x2u << 8; // PTD1/2/3 -> SCK/SOUT/SIN

    // LAN9252 shield: SCS is on Arduino D9 = PTC4. Software GPIO CS drives this pin
    // (PTC4/ALT1 = GPIO), NOT hardware SPI0_PCS0 (PTC4/ALT2). PCR4 mux + direction
    // are set privileged in the bring-up; per-transaction toggles run in the driver.
    constexpr uintptr_t PORTC_BASE = 0x4004B000u;
    constexpr uintptr_t PORTC_PCR4 = PORTC_BASE + 0x10u;

    // GPIOC (K64 RM 55.2): direct crossbar slave at 0x400F_F080, system-clocked (RM
    // 55.1.1), NOT AIPS/MPU-gated (RM 3.10.1.1) -- the unprivileged driver reaches it
    // free (the SYSMPU MMIO grant is inert; GPIO bypasses the MPU entirely).
    constexpr uintptr_t GPIOC_BASE = 0x400FF080u;
    constexpr uintptr_t GPIOC_PSOR = GPIOC_BASE + 0x04u; // set   -> PTC4 high (CS idle)
    constexpr uintptr_t GPIOC_PCOR = GPIOC_BASE + 0x08u; // clear -> PTC4 low  (CS asserted)
    constexpr uintptr_t GPIOC_PDDR = GPIOC_BASE + 0x14u; // 1 = output
    constexpr uint32_t CS_PIN = 1u << 4;                 // PTC4

    // DSPI register offsets within the granted window (RM ch.50).
    constexpr uint32_t MCR_OFFSET = 0x00u;
    constexpr uint32_t CTAR0_OFFSET = 0x0Cu;
    constexpr uint32_t SR_OFFSET = 0x2Cu;
    constexpr uint32_t PUSHR_OFFSET = 0x34u;
    constexpr uint32_t POPR_OFFSET = 0x38u;

    constexpr uint32_t MCR_MSTR = 1u << 31;
    constexpr uint32_t MCR_CLR_TXF = 1u << 11;
    constexpr uint32_t MCR_CLR_RXF = 1u << 10;
    constexpr uint32_t MCR_HALT = 1u << 0;

    // CTAR0 (RM 50.3.2). FMSZ = word_bits - 1; CPOL b26 / CPHA b25 / LSBFE b24; the
    // baud is PBR[17:16] * BR[3:0] dividers off the bus clock (derived below).
    constexpr uint32_t CTAR_CPOL = 1u << 26;
    constexpr uint32_t CTAR_CPHA = 1u << 25;
    constexpr uint32_t CTAR_LSBFE = 1u << 24;

    // PUSHR (RM 50.3.7, master): TXDATA[15:0], CTAS=0, no PCS/CONT -- the GPIO CS
    // frames the transaction, so no hardware chip select and no EOQ (the pump polls).
    // (offset PUSHR_OFFSET above.)

    // Fallback bus clock if the branch-clock oracle does not know DSPI0 (0 return):
    // K64F bus clock at the default 120 MHz core / BUS_DIV = 60 MHz. The oracle is
    // authoritative when it answers.
    constexpr uint32_t DSPI_CLK_FALLBACK = 60000000u;

    // DSPI FIFO depths (RM ch.50): 4-entry TX and RX.
    constexpr uint32_t TX_FIFO_DEPTH = 4u;
    constexpr size_t RX_FIFO_DEPTH = 4u;

    // PBR prescaler encodings (CTAR[17:16]) and BR scaler encodings (CTAR[3:0]).
    constexpr uint32_t PBR_DIV[4] = {2u, 3u, 5u, 7u};
    constexpr uint32_t BR_DIV[16] = {2u,   4u,   6u,    8u,    16u,   32u,    64u,    128u,
                                     256u, 512u, 1024u, 2048u, 4096u, 8192u, 16384u, 32768u};

    // Fold {hz, mode, word_bits} into a CTAR value off `f_periph`. Picks the PBR*BR
    // pair giving the FASTEST rate not exceeding `hz` (truthful round-DOWN), and
    // writes the achieved rate to *achieved. Baud = f_periph / (PBR * BR).
    uint32_t derive_ctar(uint32_t f_periph, uint32_t hz, uint8_t mode, uint8_t word_bits,
                         uint32_t* achieved)
    {
        uint8_t bits = word_bits;
        if (bits < 4u)
        {
            bits = 8u; // 0 (unset) or nonsense -> the 8-bit default
        }
        uint32_t ctar = (static_cast<uint32_t>(bits - 1u) & 0xFu) << 27; // FMSZ
        if ((mode & KOS_BUS_MODE_CPOL) != 0u)
        {
            ctar |= CTAR_CPOL;
        }
        if ((mode & KOS_BUS_MODE_CPHA) != 0u)
        {
            ctar |= CTAR_CPHA;
        }
        if ((mode & KOS_BUS_MODE_LSB_FIRST) != 0u)
        {
            ctar |= CTAR_LSBFE;
        }

        uint32_t best_rate = 0u;
        uint32_t best_pbr = 3u; // default slowest pair (/7, /32768) if nothing fits
        uint32_t best_br = 15u;
        for (uint32_t p = 0u; p < 4u; p++)
        {
            for (uint32_t b = 0u; b < 16u; b++)
            {
                uint32_t const rate = f_periph / (PBR_DIV[p] * BR_DIV[b]);
                if (rate <= hz and rate > best_rate)
                {
                    best_rate = rate;
                    best_pbr = p;
                    best_br = b;
                }
            }
        }
        ctar |= (best_pbr & 0x3u) << 16;
        ctar |= (best_br & 0xFu) << 0;
        if (achieved != nullptr)
        {
            *achieved = f_periph / (PBR_DIV[best_pbr] * BR_DIV[best_br]);
        }
        return ctar;
    }

    // The transaction ENGINE. No IPC, no policy beyond the CS bracket the coherent
    // transfer demands. All register access is INSIDE the granted DSPI0 window; the
    // CS is the ungated PTC4 GPIO.
    class DspiBus
    {
    public:
        void init(uintptr_t win)
        {
            win_ = win;
            cs_gpio_ = true; // the bring-up always sets PTC4 up as a GPIO CS output
        }

        // Reprogram the CTAR from a per-device config while HALTed, and adopt the CS
        // policy. Returns the achieved bit clock. Only DSPI window registers touched,
        // so the UNPRIVILEGED driver may call this (AIPS slot 44 is open).
        uint32_t configure(uint32_t hz, uint8_t mode, uint8_t word_bits, uint8_t cs_policy)
        {
            cs_gpio_ = (cs_policy == KOS_BUS_CS_GPIO);
            uint32_t f = kos_periph_clock_hz(win_);
            if (f == 0u)
            {
                f = DSPI_CLK_FALLBACK;
            }
            uint32_t achieved = 0u;
            uint32_t const ctar = derive_ctar(f, hz, mode, word_bits, &achieved);

            r32(win_ + MCR_OFFSET) = MCR_MSTR | MCR_CLR_TXF | MCR_CLR_RXF | MCR_HALT;
            r32(win_ + CTAR0_OFFSET) = ctar;
            r32(win_ + MCR_OFFSET) = MCR_MSTR; // release HALT -> RUNNING
            return achieved;
        }

        // Clock `len` bytes full-duplex: tx bytes in `buf` go out, rx overwrites
        // them in place. The CS is asserted before the first clock and released after
        // the last RX drain (Stage-D: no trailing clocked byte on release), spanning
        // ALL segments of the message -- one coherent transaction.
        void transfer(unsigned char* buf, size_t len)
        {
            if (len == 0)
            {
                return;
            }
            volatile uint32_t* sr = reinterpret_cast<volatile uint32_t*>(win_ + SR_OFFSET);
            volatile uint32_t* pushr = reinterpret_cast<volatile uint32_t*>(win_ + PUSHR_OFFSET);
            volatile uint32_t* popr = reinterpret_cast<volatile uint32_t*>(win_ + POPR_OFFSET);

            cs_low();

            // Polled full-duplex. Drain first: the 4-deep RX FIFO must never overflow
            // (a dropped byte hangs the loop). Push only while fewer than
            // RX_FIFO_DEPTH bytes are IN FLIGHT (TX FIFO + shifter + RX FIFO), so a
            // completed frame always has a free RX slot. No EOQ, no per-batch IRQ: at
            // 4-deep + ~10 MHz the FIFO cycle is shorter than a reschedule.
            size_t pushed = 0;
            size_t popped = 0;
            while (popped < len)
            {
                if (kickos::mk64f::driver::dspi_rx_count(win_) > 0u)
                {
                    buf[popped] = static_cast<unsigned char>(*popr & 0xFFu);
                    popped++;
                }
                if (pushed < len and (pushed - popped) < RX_FIFO_DEPTH
                    and ((*sr >> 12) & 0xFu) < TX_FIFO_DEPTH)
                {
                    *pushr = static_cast<uint32_t>(buf[pushed]) & 0xFFu;
                    pushed++;
                }
            }

            cs_high();
        }

    private:
        // PSOR/PCOR are write-only atomic set/clear (no RMW, no race with other PTC
        // bits). Both are Device memory, ordered with the PUSHR stores on Cortex-M4,
        // so CS-low is observed before the first SCK and CS-high after the last POPR.
        void cs_low()
        {
            if (cs_gpio_)
            {
                r32(GPIOC_PCOR) = CS_PIN;
            }
        }
        void cs_high()
        {
            if (cs_gpio_)
            {
                r32(GPIOC_PSOR) = CS_PIN;
            }
        }

        uintptr_t win_ = 0;
        bool cs_gpio_ = true;
    };

    // The DSPI0 service endpoint cap in the ROOT/init thread's table (set by the
    // bring-up, read by the app to delegate SIGNAL to its clients). -1 = not up.
    int g_spi0_ep = -1;
}

extern "C"
{
    int k64dspi_endpoint(void)
    {
        return g_spi0_ep;
    }

    // UNPRIVILEGED driver thread: owns the DSPI0 window (spawn MMIO grant) + the PTC4
    // GPIO CS (ungated). The window base arrives as the arg VALUE (never dereferenced
    // as memory). The delegated recv cap lands at child table index 1.
    void k64dspi_service(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        DspiBus bus;
        bus.init(win);

        kos::print("[k64dspi] SPI service up (DSPI0, polled FIFO, GPIO CS on PTC4)\n");

        kickos::spi::serve_loop(bus);

        kos_exit(0);
    }

    int k64dspi_spi_start(struct kos_service_cfg const* cfg)
    {
        if (cfg == nullptr or cfg->kind != KOS_SVC_SPI)
        {
            kos::print("[k64dspi] ERROR: bad or non-SPI service cfg\n");
            return -1;
        }

        uintptr_t const win_base = cfg->mmio_base;
        uint32_t const win_size = cfg->mmio_window;
        uint8_t const driver_prio = cfg->prio;

        // 1. Privileged one-time bring-up (runs in the root/init thread): the unsafe
        //    setup the unprivileged driver must NOT do. SIM (could ungate any
        //    peripheral) and PORT (could re-mux SPI onto arbitrary pins) stay
        //    privileged; the driver gets ONLY the DSPI window + the ungated GPIOC.
        r32(SIM_SCGC5) |= SCGC5_PORTD | SCGC5_PORTC;
        r32(SIM_SCGC6) |= SCGC6_SPI0;

        // SCK/SOUT/SIN on PTD1/PTD2/PTD3 (Arduino D13/D11/D12) -> ALT2. Glitch-free
        // before DSPI config only because mode 0 (CPOL=0) idle matches the pin idle
        // at mux time; a CPOL=1 device MUST program CTAR before muxing.
        r32(PORTD_PCR1) = PCR_MUX_ALT2; // SCK  (D13)
        r32(PORTD_PCR2) = PCR_MUX_ALT2; // SOUT (D11)
        r32(PORTD_PCR3) = PCR_MUX_ALT2; // SIN  (D12)

        // Software GPIO CS on PTC4 (D9): preload PDOR high, set output, THEN mux ALT1
        // (GPIO) so the pin drives high the instant it becomes an output -- CS idle
        // high, no assert glitch. Always set up (harmless when a device needs no CS;
        // the driver's cs_policy gates whether it actually toggles).
        r32(GPIOC_PSOR) = CS_PIN;
        r32(GPIOC_PDDR) |= CS_PIN;
        r32(PORTC_PCR4) = PCR_MUX_ALT1;

        // DSPI0 config while HALTed. MCR resets 0x0000_4001 (MDIS=1, HALT=1): this
        // write clears MDIS, flushes both FIFOs, sets master, holds HALT during
        // config. Then program the initial CTAR from the service cfg's target Hz.
        r32(win_base + MCR_OFFSET) = MCR_MSTR | MCR_CLR_TXF | MCR_CLR_RXF | MCR_HALT;
        uint32_t f = kos_periph_clock_hz(win_base);
        if (f == 0u)
        {
            f = DSPI_CLK_FALLBACK;
        }
        uint32_t achieved = 0u;
        uint32_t const ctar = derive_ctar(f, cfg->hz, /*mode=*/0u, /*word_bits=*/8u, &achieved);
        r32(win_base + CTAR0_OFFSET) = ctar;

        // Open DSPI0 slot 44 to user mode: clear PACR44 SP (bit 14 of AIPS0_PACRF).
        // RM 20.2 -- the ACTUAL enabler on K64F (the SYSMPU grant below is inert for
        // the peripheral). PACRF resets SP=1 (supervisor-only).
        constexpr uintptr_t AIPS0_PACRF = 0x40000044u;
        constexpr uint32_t PACR44_SP = 1u << 14;
        r32(AIPS0_PACRF) &= ~PACR44_SP;

        // Release HALT -> RUNNING; the first PUSHR starts the queue.
        r32(win_base + MCR_OFFSET) = MCR_MSTR;

        // 2. Create the request endpoint E (full rights: WAIT|SIGNAL|TRANSFER). Root
        //    KEEPS this cap so the app -- same thread, same table -- can delegate a
        //    SIGNAL-narrowed copy to each client. g_spi0_ep records the handle.
        int const ep = kos_endpoint_create();
        if (ep < 0)
        {
            kos::print("[k64dspi] ERROR: endpoint_create failed\n");
            return -1;
        }

        // 3. Spawn the UNPRIVILEGED driver: granted the DSPI0 window (R|W|DEV; inert
        //    on coarse-AIPS but kept for spawn-signature portability with PMSA/PMP)
        //    and a WAIT-only recv cap on E (child index 1). No SIGNAL/TRANSFER on the
        //    child cap: the driver receives, it does not send or re-delegate.
        int const drv = kickos::driver::spawn_unprivileged(
            k64dspi_service, win_base, win_size, cfg->name, driver_prio, ep,
            "[k64dspi] ERROR: driver spawn failed\n");
        if (drv < 0)
        {
            return -1;
        }

        g_spi0_ep = ep;
        return 0;
    }
}
