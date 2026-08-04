// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/DSPI0 SPI bus SERVICE (see <kickos/driver/k64dspi.h>). The engine derives a CTAR
// from each device's Hz/mode/word, rewrites CTAR0 at the head of every transfer, and clocks
// N bytes full-duplex through the polled 4-deep FIFO with the PTC4 GPIO CS bracketing the
// WHOLE transfer. The start shim touches no register: the block is brought up by the driver
// thread itself (kos_periph_enable), and the pin mux comes from the board pin map.
//
// Chip select is a SOFTWARE GPIO on PTC4, NOT hardware PCS0: DSPI's CONT/PCS model
// has no zero-clock CS deassert, so releasing hardware PCS0 clocked a trailing dummy
// byte that corrupted length-sensitive LAN9252 mailbox writes. The GPIO write path is
// ungated (K64 RM 3.10.1.1: GPIO is a direct crossbar slave with no PACR and no SYSMPU
// coverage), so the unprivileged driver sets PTC4's direction and toggles PSOR/PCOR with
// no grant of its own.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/k64dspi.h>

#include <kickos/sys/bus.h>         // kos_bus_req/seg/rsp/cfg wire ABI
#include <kickos/sys/service.h>     // kos_service_cfg (base/window/prio/cs as data)
#include <kickos/sys/bytes.h>          // mem_copy
#include <kickos/sys/spi_service.h>    // kickos::spi::serve_loop (shared choreography)
#include <kickos/sys/driver_bringup.h> // kickos::driver::spawn_unprivileged
#include <kickos/sys/emit.h>           // publish-aware write (kos_print is dropped once published)
#include <kickos/io/mmio.h>            // r32

#include <dspi_class.h> // Rule 6 class-driver leaf: shared DSPI RX-FIFO fill-level read

#include <stdint.h>
#include <stddef.h>

namespace
{
    // DSPI0 / GPIO register map (K64 RM ch.50, 55.2).

    // LAN9252 shield: SCS is on Arduino D9 = PTC4, muxed to GPIO (PTC4/ALT1) by the
    // board pin map, NOT to hardware SPI0_PCS0 (PTC4/ALT2).
    //
    // GPIOC (K64 RM 55.2): direct crossbar slave at 0x400F_F080, system-clocked (RM
    // 55.1.1), NOT AIPS/MPU-gated (RM 3.10.1.1). The unprivileged driver reaches it
    // free, because GPIO bypasses the MPU entirely and the SYSMPU MMIO grant is inert.
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

    // PUSHR (RM 50.3.7, master): TXDATA[15:0], CTAS=0, no PCS/CONT. The GPIO CS frames the
    // transaction, so no hardware chip select and no EOQ (the pump polls).

    // Fallback bus clock if the branch-clock oracle does not know DSPI0 (a 0 return):
    // K64F bus clock at the default 120 MHz core / BUS_DIV = 60 MHz.
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

    // The transaction engine: no IPC, all register access INSIDE the granted DSPI0 window,
    // and the CS on the ungated PTC4 GPIO.
    class DspiBus
    {
    public:
        // The CTAR word one device's config folds down to. CTAR0 is the controller's
        // single live profile register, so it is rewritten inside every transfer
        // rather than once at CONFIG time.
        struct Profile
        {
            uint32_t ctar;
            bool cs_gpio;
        };

        void init(uintptr_t win)
        {
            win_ = win;
        }

        // Fold a per-device config into a CTAR word; returns the achieved bit clock. Rate,
        // CPOL/CPHA, word size and bit order all land in CTAR0, which is inside the granted
        // window, so every one of them is per-device on this chip.
        uint32_t fold(struct kos_bus_cfg const& cfg, Profile& out)
        {
            out.cs_gpio = (cfg.cs_policy == KOS_BUS_CS_GPIO);
            uint32_t f = kos_periph_clock_hz(win_);
            if (f == 0u)
            {
                f = DSPI_CLK_FALLBACK;
            }
            uint32_t achieved = 0u;
            out.ctar = derive_ctar(f, cfg.hz, cfg.mode, cfg.word_bits, &achieved);
            return achieved;
        }

        // Apply device `p`'s CTAR, then clock `len` bytes full-duplex: tx bytes in `buf` go
        // out, rx overwrites them in place. The CS is asserted before the first clock and
        // released after the last RX drain, so no clocked byte trails the release, and ONE
        // transaction spans ALL segments of the message. Only DSPI window registers are
        // touched, so the UNPRIVILEGED driver may do this (AIPS slot 44 is open).
        void transfer(Profile const& p, unsigned char* buf, size_t len)
        {
            if (len == 0)
            {
                return;
            }
            // CTAR0 is writable only while HALTed; the same write flushes both FIFOs,
            // which must already be drained here (the pump below pops all it pushes).
            // SCK idles at the PREVIOUS profile's CPOL, so a CPOL=1 device sees one
            // idle-level change here, before its own CS asserts.
            r32(win_ + MCR_OFFSET) = MCR_MSTR | MCR_CLR_TXF | MCR_CLR_RXF | MCR_HALT;
            r32(win_ + CTAR0_OFFSET) = p.ctar;
            r32(win_ + MCR_OFFSET) = MCR_MSTR; // release HALT -> RUNNING

            volatile uint32_t* sr = reinterpret_cast<volatile uint32_t*>(win_ + SR_OFFSET);
            volatile uint32_t* pushr = reinterpret_cast<volatile uint32_t*>(win_ + PUSHR_OFFSET);
            volatile uint32_t* popr = reinterpret_cast<volatile uint32_t*>(win_ + POPR_OFFSET);

            cs_low(p.cs_gpio);

            // Polled full-duplex. Drain first: the 4-deep RX FIFO must never overflow, or
            // the dropped byte hangs this loop. Push only while fewer than RX_FIFO_DEPTH
            // bytes are IN FLIGHT (TX FIFO + shifter + RX FIFO), so a completed frame
            // always has a free RX slot.
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

            cs_high(p.cs_gpio);
        }

    private:
        // PSOR/PCOR are write-only atomic set/clear (no RMW, no race with other PTC
        // bits). Both are Device memory, ordered with the PUSHR stores on Cortex-M4,
        // so CS-low is observed before the first SCK and CS-high after the last POPR.
        void cs_low(bool on)
        {
            if (on)
            {
                r32(GPIOC_PCOR) = CS_PIN;
            }
        }
        void cs_high(bool on)
        {
            if (on)
            {
                r32(GPIOC_PSOR) = CS_PIN;
            }
        }

        uintptr_t win_ = 0;
    };

    // The DSPI0 service endpoint cap in the ROOT/init thread's table (set by the
    // bring-up, taken ONCE by the app to delegate SIGNAL to its single client).
    // -1 = not up, or already taken.
    kos_cap_t g_spi0_ep = KOS_CAP_NONE;
}

extern "C"
{
    kos_cap_t k64dspi_take_endpoint(void)
    {
        kos_cap_t const ep = g_spi0_ep;
        g_spi0_ep = KOS_CAP_NONE; // one-shot: device slots are caller-named, so ONE client only
        return ep;
    }

    // UNPRIVILEGED driver thread: owns the DSPI0 window (spawn MMIO grant) + the PTC4
    // GPIO CS (ungated). The window base arrives as the arg VALUE (never dereferenced
    // as memory). The delegated recv cap lands at child table index 1.
    void k64dspi_service(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        // Software GPIO CS on PTC4: preload PDOR high, THEN switch the pin to output, so it
        // drives high the instant it becomes one and cannot glitch an assert.
        //
        // BEFORE the periph_enable check: GPIO needs no PACR and no grant, so it does not
        // depend on the seam. Ordered after the check, a refusal would exit with the pin a
        // floating input (PDDR resets to 0).
        r32(GPIOC_PSOR) = CS_PIN;
        r32(GPIOC_PDDR) |= CS_PIN;

        // Ungate SPI0 and clear DSPI0's AIPS0 slot 44 SP bit (RM 20.2) through the grant
        // this thread holds. Until it returns, the window reads supervisor-only and the
        // block is unclocked. The diagnostic goes through emit, not kos_print: the console
        // is already USER_OWNED here, so the kernel chip path drops every byte.
        if (kos_periph_enable(win) != 0)
        {
            kickos::emit("[k64dspi] ERROR: periph_enable failed, DSPI0 unreachable\n");
            kos_exit(-1);
        }

        // DSPI0 out of reset while HALTed. MCR resets 0x0000_4001 (MDIS=1, HALT=1):
        // this write clears MDIS, flushes both FIFOs and sets master, then HALT is
        // released. CTAR0 keeps its reset value (CPOL=0, so SCK idles low, matching the
        // mode-0 pin idle); every transfer rewrites it from the named device's profile,
        // and an XFER without a folded profile is refused, so no transfer ever runs on
        // the reset CTAR.
        r32(win + MCR_OFFSET) = MCR_MSTR | MCR_CLR_TXF | MCR_CLR_RXF | MCR_HALT;
        r32(win + MCR_OFFSET) = MCR_MSTR;

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

        // 1. Create the request endpoint E (full rights: WAIT|SIGNAL|TRANSFER). Root KEEPS
        //    this cap so the app, on the same thread and table, can delegate a
        //    SIGNAL-narrowed copy to each client. g_spi0_ep records the handle.
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            kos::print("[k64dspi] ERROR: endpoint_create failed\n");
            return -1;
        }

        // 2. Spawn the UNPRIVILEGED driver: granted the DSPI0 window (R|W|DEV, inert on
        //    coarse-AIPS for the peripheral itself, but it is what AUTHORISES the driver's
        //    kos_periph_enable) and a WAIT-only recv cap on E (child index 1). No
        //    SIGNAL/TRANSFER on the child cap: the driver receives, it does not send or
        //    re-delegate.
        auto const drv = kickos::driver::spawn_unprivileged(
            k64dspi_service, win_base, win_size, cfg->name, driver_prio, ep,
            "[k64dspi] ERROR: driver spawn failed\n");
        if (not drv.valid())
        {
            return -1;
        }

        g_spi0_ep = ep;
        return 0;
    }
}
