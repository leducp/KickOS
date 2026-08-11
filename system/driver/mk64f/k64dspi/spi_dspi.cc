// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K64F DSPI0 backend of the SPI class <kickos/driver/spi.h>: the block bring-up plus the
// polled full-duplex FIFO engine, for a thread that ALREADY HOLDS the DSPI0 register window.
//
// CHIP SELECT IS A SOFTWARE GPIO ON PTC4, NOT hardware PCS0: DSPI's CONT/PCS model has no
// zero-clock CS deassert, so releasing hardware PCS0 clocked a trailing dummy byte that
// corrupted length-sensitive LAN9252 mailbox writes. The GPIO write path is ungated (K64 RM
// 3.10.1.1: a direct crossbar slave with no PACR and no SYSMPU coverage), so the unprivileged
// owner sets PTC4's direction and toggles PSOR/PCOR with no grant of its own. The PTC4 pin MUX
// is NOT set here; it comes from the board pin map.
//
// Register addresses / bit fields are clean-room from the K64 Sub-Family Reference Manual;
// "RM ch.NN" are its printed chapters.

#include <kickos/driver/spi.h>

#include <kickos/io/mmio.h>   // r32
#include <kickos/sys.h>       // kos_periph_enable, kos_periph_clock_hz
#include <kickos/sys/errno.h> // KOS_EPERM, KOS_EINVAL, KOS_ENOSYS, KOS_ENOTSUP

#include <dspi_class.h> // shared DSPI RX-FIFO fill-level read

#include <stdint.h>

namespace
{
    // LAN9252 shield: SCS is on Arduino D9 = PTC4, muxed to GPIO (PTC4/ALT1) by the board pin
    // map, NOT to hardware SPI0_PCS0 (PTC4/ALT2).
    //
    // GPIOC (K64 RM 55.2) is a direct crossbar slave at 0x400F_F080, system-clocked (RM
    // 55.1.1) and NOT AIPS/MPU-gated (RM 3.10.1.1), so the unprivileged owner reaches it free.
    // That is about GPIOC ONLY: the DSPI0 window grant is load-bearing
    // (docs/reference/boards.md, "When an MMIO grant is INERT").
    constexpr uintptr_t GPIOC_BASE = 0x400FF080u;
    constexpr uintptr_t GPIOC_PSOR = GPIOC_BASE + 0x04u; // set   -> PTC4 high (CS idle)
    constexpr uintptr_t GPIOC_PCOR = GPIOC_BASE + 0x08u; // clear -> PTC4 low  (CS asserted)
    constexpr uintptr_t GPIOC_PDDR = GPIOC_BASE + 0x14u; // 1 = output
    constexpr uint32_t CS_PIN = 1u << 4;                 // PTC4

    // DSPI register offsets within the granted window (RM ch.50).
    constexpr uintptr_t MCR_OFFSET = 0x00u;
    constexpr uintptr_t CTAR0_OFFSET = 0x0Cu;
    constexpr uintptr_t SR_OFFSET = 0x2Cu;
    constexpr uintptr_t PUSHR_OFFSET = 0x34u;
    constexpr uintptr_t POPR_OFFSET = 0x38u;

    constexpr uint32_t MCR_MSTR = 1u << 31;
    constexpr uint32_t MCR_CLR_TXF = 1u << 11;
    constexpr uint32_t MCR_CLR_RXF = 1u << 10;
    constexpr uint32_t MCR_HALT = 1u << 0;

    // CTAR0 (RM 50.3.2). DBR b31 doubles the rate; FMSZ[30:27] = word_bits - 1; CPOL b26 /
    // CPHA b25 / LSBFE b24; the baud is PBR[17:16] * BR[3:0] off the bus clock.
    constexpr uint32_t CTAR_DBR = 1u << 31;
    constexpr uint32_t CTAR_CPOL = 1u << 26;
    constexpr uint32_t CTAR_CPHA = 1u << 25;
    constexpr uint32_t CTAR_LSBFE = 1u << 24;
    constexpr unsigned CTAR_FMSZ_SHIFT = 27u;
    constexpr unsigned CTAR_PBR_SHIFT = 16u;

    // PUSHR (RM 50.3.7, master): TXDATA[15:0], CTAS=0, no PCS/CONT, no EOQ.

    // DSPI FIFO depths (RM ch.50): 4-entry TX and RX.
    constexpr uint32_t TX_FIFO_DEPTH = 4u;
    constexpr uint32_t RX_FIFO_DEPTH = 4u;

    // PBR prescaler encodings (CTAR[17:16]) and BR scaler encodings (CTAR[3:0]).
    constexpr uint32_t PBR_DIV[4] = {2u, 3u, 5u, 7u};
    constexpr uint32_t BR_DIV[16] = {2u,   4u,   6u,    8u,    16u,   32u,    64u,    128u,
                                     256u, 512u, 1024u, 2048u, 4096u, 8192u, 16384u, 32768u};

    // The one frame size this engine clocks. The class buffer is one byte per frame, so a
    // wider FMSZ would leave the top bits of every frame unsourced.
    constexpr uint8_t FRAME_WORD_BITS = 8u;

    // kos_spi_device.prog slots.
    constexpr unsigned PROG_CTAR = 0u;

    // The rate a CTAR word produces off `f_periph`: f * (1 + DBR) / (PBR * BR).
    uint32_t ctar_rate(uint32_t f_periph, uint32_t ctar)
    {
        uint32_t const pbr = PBR_DIV[(ctar >> CTAR_PBR_SHIFT) & 0x3u];
        uint32_t const br = BR_DIV[ctar & 0xFu];
        uint32_t mult = 1u;
        if ((ctar & CTAR_DBR) != 0u)
        {
            mult = 2u;
        }
        return (f_periph * mult) / (pbr * br);
    }

    // The CTAR baud fields for the FASTEST PBR*BR pair not exceeding `hz`. A `hz` below the
    // slowest pair keeps that pair, so the caller sees a rate ABOVE its request and must
    // refuse it.
    uint32_t derive_baud_fields(uint32_t f_periph, uint32_t hz)
    {
        uint32_t best_rate = 0u;
        uint32_t best_pbr = 3u; // slowest pair (/7, /32768) if nothing fits
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
        return ((best_pbr & 0x3u) << CTAR_PBR_SHIFT) | (best_br & 0xFu);
    }

    void cs_low(bool on)
    {
        // PSOR/PCOR are write-only atomic set/clear: no RMW, so no race with other PTC bits.
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
}

extern "C"
{

int32_t kos_spi_bus_open(struct kos_spi_bus* b, struct kos_spi_bus_config const* cfg)
{
    if (cfg->base == 0u)
    {
        return -KOS_EINVAL;
    }
    b->base = cfg->base;
    b->ep = cfg->ep;
    b->irq = cfg->irq; // the pump polls; recorded so the POD carries what it was opened with

    // Preload PDOR high, THEN switch the pin to output, so it cannot glitch an assert. Kept
    // BEFORE the periph_enable check: ordered after it, a refusal would leave the pin a
    // floating input (PDDR resets to 0).
    r32(GPIOC_PSOR) = CS_PIN;
    r32(GPIOC_PDDR) |= CS_PIN;

    // Until this returns, the window reads supervisor-only and the block is unclocked: it
    // ungates SPI0 and clears DSPI0's AIPS0 slot 44 SP bit (RM 20.2).
    if (kos_periph_enable(b->base) != 0)
    {
        return -KOS_EPERM;
    }

    // MCR resets to 0x0000_4001 (MDIS=1, HALT=1): the first write clears MDIS, flushes both
    // FIFOs and sets master, the second releases HALT. CTAR0 keeps its reset value until a
    // transfer rewrites it from the named device's profile.
    r32(b->base + MCR_OFFSET) = MCR_MSTR | MCR_CLR_TXF | MCR_CLR_RXF | MCR_HALT;
    r32(b->base + MCR_OFFSET) = MCR_MSTR;
    return 0;
}

int32_t kos_spi_device_open(struct kos_spi_device* d, struct kos_spi_bus* b,
                            struct kos_spi_device_config const* cfg)
{
    if (b->base == 0u)
    {
        return -KOS_EINVAL; // no open bus behind this handle
    }
    if (cfg->slot >= KOS_BUS_DEV_MAX)
    {
        return -KOS_EINVAL;
    }
    if (cfg->rsv[0] != 0u or cfg->rsv[1] != 0u or cfg->rsv[2] != 0u)
    {
        return -KOS_EINVAL;
    }
    if ((cfg->mode & ~static_cast<uint8_t>(KOS_BUS_MODE_CPOL | KOS_BUS_MODE_CPHA
                                          | KOS_BUS_MODE_LSB_FIRST)) != 0u)
    {
        return -KOS_EINVAL;
    }

    uint8_t bits = cfg->word_bits;
    if (bits == 0u)
    {
        bits = FRAME_WORD_BITS;
    }
    if (bits != FRAME_WORD_BITS)
    {
        return -KOS_ENOTSUP;
    }

    // ONE CS PIN, PTC4: a non-zero cs_index is a pin this driver does not drive.
    if (cfg->cs_index != 0u)
    {
        return -KOS_ENOTSUP;
    }
    if (cfg->cs_policy == KOS_BUS_CS_HW)
    {
        // Hardware PCS0 has no zero-clock deassert, so it cannot bracket a transaction
        // without clocking a trailing byte.
        return -KOS_ENOTSUP;
    }
    if (cfg->cs_policy != KOS_BUS_CS_GPIO and cfg->cs_policy != KOS_BUS_CS_NONE)
    {
        return -KOS_EINVAL;
    }

    uint32_t f = kos_periph_clock_hz(b->base);
    if (f == 0u)
    {
        return -KOS_ENOSYS; // no branch-clock oracle for this block: the rate is unknowable
    }

    uint32_t ctar = (static_cast<uint32_t>(bits - 1u) & 0xFu) << CTAR_FMSZ_SHIFT;
    if ((cfg->mode & KOS_BUS_MODE_CPOL) != 0u)
    {
        ctar |= CTAR_CPOL;
    }
    if ((cfg->mode & KOS_BUS_MODE_CPHA) != 0u)
    {
        ctar |= CTAR_CPHA;
    }
    if ((cfg->mode & KOS_BUS_MODE_LSB_FIRST) != 0u)
    {
        ctar |= CTAR_LSBFE;
    }

    if (cfg->hz == 0u)
    {
        // Keep the LIVE CTAR0 baud fields and report what they produce. Before any device has
        // been opened on this bus those are CTAR0's RESET fields (/2 * /2), a rate genuinely
        // read back rather than one anybody negotiated.
        ctar |= r32(b->base + CTAR0_OFFSET) & ((0x3u << CTAR_PBR_SHIFT) | 0xFu | CTAR_DBR);
    }
    else
    {
        ctar |= derive_baud_fields(f, cfg->hz);
    }
    uint32_t const rate = ctar_rate(f, ctar);
    if (rate == 0u)
    {
        return -KOS_ENOTSUP;
    }
    if (cfg->hz != 0u and rate > cfg->hz)
    {
        // The divider cannot reach that low, and clocking the device faster than it asked is
        // not a rounding difference.
        return -KOS_ENOTSUP;
    }

    d->bus = b;
    d->hz = rate;
    d->prog[PROG_CTAR] = ctar;
    d->prog[1] = 0u;
    d->slot = cfg->slot;
    d->mode = cfg->mode;
    d->word_bits = bits;
    d->cs_policy = cfg->cs_policy;
    d->cs_index = cfg->cs_index;
    d->rsv[0] = 0u;
    d->rsv[1] = 0u;
    d->rsv[2] = 0u;
    return static_cast<int32_t>(rate);
}

int32_t kos_spi_transfer(struct kos_spi_device* d, struct kos_bus_seg const* seg, uint8_t nseg,
                         unsigned char* buf, uint32_t len)
{
    int32_t const bad = kos_spi_seg_check(seg, nseg, len);
    if (bad != 0)
    {
        return bad;
    }
    struct kos_spi_bus* const b = d->bus;
    if (b == nullptr or b->base == 0u)
    {
        return -KOS_EINVAL;
    }
    uintptr_t const win = b->base;
    bool const cs_gpio = (d->cs_policy == KOS_BUS_CS_GPIO);

    // CTAR0 is writable only while HALTed, and the same write flushes both FIFOs, which must
    // already be drained here (the pump below pops all it pushes). SCK idles at the PREVIOUS
    // profile's CPOL, so a CPOL=1 device sees one idle-level change before its own CS asserts.
    r32(win + MCR_OFFSET) = MCR_MSTR | MCR_CLR_TXF | MCR_CLR_RXF | MCR_HALT;
    r32(win + CTAR0_OFFSET) = d->prog[PROG_CTAR];
    r32(win + MCR_OFFSET) = MCR_MSTR; // release HALT -> RUNNING

    volatile uint32_t* sr = reinterpret_cast<volatile uint32_t*>(win + SR_OFFSET);
    volatile uint32_t* pushr = reinterpret_cast<volatile uint32_t*>(win + PUSHR_OFFSET);
    volatile uint32_t* popr = reinterpret_cast<volatile uint32_t*>(win + POPR_OFFSET);

    cs_low(cs_gpio);

    // Drain FIRST: the 4-deep RX FIFO must never overflow, or the dropped byte hangs this
    // loop. Push only while fewer than RX_FIFO_DEPTH bytes are IN FLIGHT (TX FIFO + shifter +
    // RX FIFO), so a completed frame always has a free RX slot.
    uint32_t pushed = 0u;
    uint32_t popped = 0u;
    while (popped < len)
    {
        if (kickos::mk64f::driver::dspi_rx_count(win) > 0u)
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

    cs_high(cs_gpio);
    return static_cast<int32_t>(len);
}

int32_t kos_spi_bus_close(struct kos_spi_bus* b)
{
    if (b->base == 0u)
    {
        return 0; // never opened, or already closed
    }
    // HALT stops the clock generator with both FIFOs flushed. MDIS is left CLEAR so the window
    // stays readable for a later reopen.
    r32(b->base + MCR_OFFSET) = MCR_MSTR | MCR_CLR_TXF | MCR_CLR_RXF | MCR_HALT;
    r32(GPIOC_PSOR) = CS_PIN; // CS back to its idle level
    b->base = 0u;
    return 0;
}

}
