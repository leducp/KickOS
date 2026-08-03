// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800/USIC0-CH1 SSC (SPI) bus SERVICE (see <kickos/driver/xmcssc.h>).
//
// The transaction engine folds each device's config into SCTR (word size, bit order) and
// PCR (the SSC-master CS framing), rewrites both at the head of every transfer, and clocks
// N bytes one word at a time, blocking on the USIC0 SR1 RX-complete IRQ per word. For
// KOS_BUS_CS_HW the hardware MSLS/SELO0 line brackets the WHOLE transfer: PCR.FEM=1 holds
// MSLS asserted across the software gaps between words (RM 18.4.5.1), TCSR.SOF marks word
// 0 and TCSR.EOF the last, so the slave sees one coherent frame.
//
// The data path is INTERNAL LOOP-BACK (DX0 = own transmitter, input "G", RM 18.2.3.5), so
// rx == tx on-chip and SELO0 is armed but never routed to a pin; the IOCR pin-mux stays
// privileged and untouched.
//
// The bring-up sets the baud profile (FDR/BRG), input routing (INPR) and channel enable
// (CCR) once; the engine folds SCTR/TCSR/PCR per device. Everything sits inside the granted
// 0x200 window, but FDR/BRG/CCR are write-PV-only at the bus and an unprivileged store to
// them is silently discarded with NO fault (measured: user/apps/xmc4800-relax/pvprobe), so
// those three go through kos_periph_reg_write and are READ BACK; a discarded store is
// invisible otherwise.
//
// CPOL/CPHA (BRG.SCLKCFG) are fixed to SPI mode 0 at bring-up, so a device profile carries
// only bit order, word size and the CS framing.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference
// Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/xmcssc.h>

#include <kickos/sys/bus.h>         // kos_bus_req/seg/rsp/cfg wire ABI
#include <kickos/sys/service.h>     // kos_service_cfg (base/window/prio/cs as data)
#include <kickos/sys/bytes.h>          // mem_copy
#include <kickos/sys/spi_service.h>    // kickos::spi::serve_loop (shared choreography)
#include <kickos/sys/driver_bringup.h> // kickos::driver::spawn_unprivileged
#include <kickos/sys/emit.h>           // publish-aware write (kos_print is dropped once published)
#include <kickos/io/mmio.h>            // r32

#include <regs/usic.h> // shared XMC USIC register offsets + SSC bit fields

#include <stdint.h>
#include <stddef.h>

using namespace kickos::xmc::reg::usic;

namespace
{
    // The console owns U0C0 (0x4003_0000); SPI uses the sibling channel U0C1
    // (0x4003_0200), whose base arrives as the service cfg's mmio_base.

    // The fixed baud profile as plain integers, mirroring the FDR/BRG register words in the
    // shared header (FDR_STEP_367 / BRG_*). Used only by profile_bitclock below.
    constexpr uint32_t BAUD_STEP = 367u;
    constexpr uint32_t BAUD_PDIV_PLUS1 = 14u;
    constexpr uint32_t BAUD_DCTQ_PLUS1 = 16u;

    // SCTR field positions for the client-configurable word size (FLE/WLE are N+1).
    constexpr unsigned SCTR_FLE_SHIFT = 16u;
    constexpr unsigned SCTR_WLE_SHIFT = 24u;

    // TCSR frame-control base (start-on-TDV + single-shot); SOF/EOF added per word.
    constexpr uint32_t TCSR_BASE = TCSR_TDEN_TDV | TCSR_TDSSM;

    // PCR hardware-CS base: SSC master, direct active-low SELO0, plus FEM to hold the
    // CS across a multi-word frame (RM 18.4.5.1).
    constexpr uint32_t PCR_CS_HW = PCR_MSLSEN | PCR_SELCTR_DIRECT | PCR_SELINV_LOW | PCR_SELO0 | PCR_FEM;

    // Clear both RX-complete flags (a single-word frame lands in AIF, RM 18.4.2.7).
    constexpr uint32_t PSCR_CLEAR_RX = PSCR_CRIF | PSCR_CAIF;

    constexpr int USIC0_SR1_IRQ = 85; // RM Table 4-3

    // Fallback peripheral clock if the branch-clock oracle does not know USIC0 (a 0
    // return): XMC4800 peripheral bus clock at 144 MHz. Used ONLY to report the nominal
    // profile rate; the profile dividers are fixed regardless.
    constexpr uint32_t USIC_CLK_FALLBACK = 144000000u;

    // Nominal SSC bit clock of the fixed bring-up profile off `f_periph`: fractional
    // divider output fFD = f_periph * STEP/1024, then /(PDIV+1)/2/(DCTQ+1). Informational
    // only; the client cannot retune it, because configure() never refolds FDR/BRG.
    uint32_t profile_bitclock(uint32_t f_periph)
    {
        uint64_t f_fd = (static_cast<uint64_t>(f_periph) * BAUD_STEP) / 1024u;
        uint64_t rate = f_fd / BAUD_PDIV_PLUS1 / 2u / BAUD_DCTQ_PLUS1;
        return static_cast<uint32_t>(rate);
    }

    // The transaction engine: no IPC, and all register access INSIDE the granted U0C1
    // window. The per-word RX-complete is the USIC0 SR1 IRQ.
    class UsicSscBus
    {
    public:
        // The SCTR/PCR words one device's config folds down to. SCTR/PCR are the
        // channel's single live profile register set, so these are rewritten inside
        // every transfer rather than once at CONFIG time.
        struct Profile
        {
            uint32_t sctr;
            uint32_t pcr;
            bool cs_hw;
        };

        void init(uintptr_t win, int irq)
        {
            win_ = win;
            irq_ = irq;
        }

        // Fold a per-device config into the SCTR (word size + bit order) and PCR (the
        // SSC CS framing) words; returns the channel's nominal bit clock. cfg.hz is
        // DISCARDED because FDR/BRG are write-PV-only and the bus drops the store here,
        // and CPOL/CPHA (BRG.SCLKCFG) are fixed to mode 0 at bring-up for the same reason.
        uint32_t fold(struct kos_bus_cfg const& cfg, Profile& out)
        {
            out.cs_hw = (cfg.cs_policy == KOS_BUS_CS_HW);

            uint8_t bits = cfg.word_bits;
            if (bits < 4u)
            {
                bits = 8u; // 0 (unset) or nonsense -> the 8-bit default
            }
            uint32_t sctr = SCTR_TRM_ACTIVE | ((static_cast<uint32_t>(bits - 1u) & 0xFu) << SCTR_WLE_SHIFT);
            if (out.cs_hw)
            {
                // FLE=63: the frame is NOT terminated by a bit count; the software
                // TCSR.SOF/EOF markers govern the frame end (RM 18.4.3.6), so MSLS
                // brackets all the words the transfer feeds.
                sctr |= SCTR_FLE_63;
            }
            else
            {
                sctr |= (static_cast<uint32_t>(bits - 1u) & 0x3Fu) << SCTR_FLE_SHIFT; // one word per frame
            }
            if ((cfg.mode & KOS_BUS_MODE_LSB_FIRST) == 0u)
            {
                sctr |= SCTR_SDIR_MSB;
            }
            out.sctr = sctr;

            out.pcr = PCR_MSLSEN;
            if (out.cs_hw)
            {
                out.pcr = PCR_CS_HW;
            }

            uint32_t f = kos_periph_clock_hz(win_);
            if (f == 0u)
            {
                f = USIC_CLK_FALLBACK;
            }
            return profile_bitclock(f);
        }

        // Apply device `p`'s profile to the live SCTR/PCR, then clock `len` bytes one
        // word at a time; tx bytes in `buf` go out, rx overwrites them in place. For
        // CS_HW the MSLS/SELO0 line asserts on word 0 (SOF) and, held by FEM across the
        // software gaps, releases only after the last word (EOF), so ONE transaction
        // spans ALL segments of the message.
        void transfer(Profile const& p, unsigned char* buf, size_t len)
        {
            if (len == 0)
            {
                return;
            }
            r32(win_ + off::SCTR) = p.sctr;
            r32(win_ + off::PCR) = p.pcr;

            volatile uint32_t* tcsr = reinterpret_cast<volatile uint32_t*>(win_ + off::TCSR);
            volatile uint32_t* pscr = reinterpret_cast<volatile uint32_t*>(win_ + off::PSCR);
            volatile uint32_t* rbuf = reinterpret_cast<volatile uint32_t*>(win_ + off::RBUF);
            volatile uint32_t* tbuf0 = reinterpret_cast<volatile uint32_t*>(win_ + off::TBUF0);

            for (size_t i = 0; i < len; i++)
            {
                // Frame markers must be set BEFORE the TBUF write (sampled when TDV
                // goes valid). Only meaningful for CS_HW (FLE=63); harmless otherwise.
                uint32_t tcsr_val = TCSR_BASE;
                if (p.cs_hw)
                {
                    if (i == 0)
                    {
                        tcsr_val |= TCSR_SOF;
                    }
                    if (i == (len - 1u))
                    {
                        tcsr_val |= TCSR_EOF;
                    }
                }
                *tcsr = tcsr_val;

                *tbuf0 = static_cast<uint32_t>(buf[i]) & 0xFFu; // TDV=1 -> clock one frame

                kos_irq_wait(irq_);                          // block until AIF/RIF raises SR1
                buf[i] = static_cast<unsigned char>(*rbuf & 0xFFu); // read releases RBUF

                *pscr = PSCR_CLEAR_RX; // W1C AIF/RIF BEFORE re-arm: an un-cleared level
                                       // re-asserts SR1 on unmask and storms it.
                kos_irq_ack(irq_);     // unmask NVIC 85 (flag already clear -> no storm)
            }

            if (p.cs_hw)
            {
                *pscr = PSCR_MSLS; // defensively force MSLS inactive after the EOF word
            }
        }

    private:
        uintptr_t win_ = 0;
        int irq_ = -1;
    };

    // The SSC service endpoint cap in the ROOT/init thread's table (set by the
    // bring-up, taken ONCE by the app to delegate SIGNAL to its single client).
    // -1 = not up, or already taken.
    int g_spi0_ep = -1;

    constexpr uint32_t FDR_WORD = FDR_DM_FRACTIONAL | FDR_STEP_367;
    constexpr uint32_t BRG_WORD = BRG_PDIV_13 | BRG_DCTQ_15 | BRG_PCTQ_0;
    constexpr uint32_t CCR_WORD = CCR_MODE_SSC | CCR_RIEN | CCR_AIEN;

    // FDR/BRG read back exactly what was written apart from FDR.RESULT[25:16], which
    // the fractional divider drives; BRG carries no read-only field in the bits this
    // profile sets.
    constexpr uint32_t FDR_RESULT_MASK = 0x03FF0000u;

    // Write a PV-classified register through the seam, then read it back: a dropped store
    // leaves the register at its previous value with no fault, so the read-back is the only
    // evidence either way.
    bool priv_write_verify(uintptr_t win, uintptr_t offset, uint32_t value, uint32_t care)
    {
        if (kos_periph_reg_write(win, offset, value) != 0)
        {
            return false;
        }
        return (r32(win + offset) & care) == (value & care);
    }

    // One-time channel bring-up, run by the UNPRIVILEGED driver thread that holds the U0C1
    // window. The RM fixes the order: KSCFG first (with MODEN=0 the channel is inaccessible
    // except through KSCFG), then every configuration register while CCR.MODE=0, then CCR
    // last to enable the channel.
    bool bring_up(uintptr_t win)
    {
        r32(win + off::KSCFG) = KSCFG_MODEN | KSCFG_BPMODEN;
        // RM p.18-165: read KSCFG back before touching other USIC registers to flush
        // the control-block pipeline; keep the volatile read from being elided.
        uint32_t const kscfg_sync = r32(win + off::KSCFG);
        __asm volatile("" : : "r"(kscfg_sync) : "memory");

        if (not priv_write_verify(win, off::FDR, FDR_WORD, ~FDR_RESULT_MASK))
        {
            kickos::emit("[xmcssc] ERROR: FDR privileged write refused or discarded\n");
            return false;
        }
        if (not priv_write_verify(win, off::BRG, BRG_WORD, 0xFFFFFFFFu))
        {
            kickos::emit("[xmcssc] ERROR: BRG privileged write refused or discarded\n");
            return false;
        }

        // Seed only: the engine rewrites SCTR/PCR from the named device's profile at the
        // head of every transfer, and an XFER on a slot with no CONFIG is refused, so no
        // transfer ever runs on these values.
        r32(win + off::SCTR) = SCTR_TRM_ACTIVE | (7u << SCTR_WLE_SHIFT) | SCTR_SDIR_MSB
                             | (7u << SCTR_FLE_SHIFT);
        r32(win + off::TCSR) = TCSR_BASE;
        r32(win + off::PCR) = PCR_MSLSEN;
        r32(win + off::PSCR) = PSCR_MSLS | PSCR_CLEAR_RX; // clear stale flags

        // Internal loop-back: DX0 receives the channel's own transmitter (input "G")
        // routed to the data shift unit. Input-stage config must be done while
        // CCR.MODE=0 (RM p.18-57). No external device on the bench -> rx == tx.
        r32(win + off::DX0CR) = DX0CR_INSW | DX0CR_DSEL_G;

        // Route the receive / alternative-receive interrupts to service-request SR1
        // (NVIC 85).
        r32(win + off::INPR) = INPR_RINP_SR1 | INPR_AINP_SR1;

        if (not priv_write_verify(win, off::CCR, CCR_WORD, 0xFFFFFFFFu))
        {
            kickos::emit("[xmcssc] ERROR: CCR privileged write refused or discarded\n");
            return false;
        }

        // No IOCR pin-mux: the data path is internal loopback and SELO0 is never routed
        // to a pin, so the IOCR escalation surface is not even touched.
        return true;
    }
}

extern "C"
{
    int xmc_spi0_take_endpoint(void)
    {
        int const ep = g_spi0_ep;
        g_spi0_ep = -1; // one-shot: device slots are caller-named, so ONE client only
        return ep;
    }

    // UNPRIVILEGED driver thread: owns the U0C1 window (spawn MMIO grant) and the USIC0 SR1
    // IRQ line, whose cap start() claimed and delegated. The window base arrives as the arg
    // VALUE, never dereferenced as memory. Diagnostics go through emit, not kos_print: the
    // console is already USER_OWNED here, so the kernel chip path drops every byte.
    //
    // NO FAILURE PATH MAY kos_exit: root KEEPS a WAIT-bearing cap on E, so recv_holders
    // never reaches 0 when this thread dies, the last-receiver-gone -KOS_EPIPE wake never
    // fires, and a client parked in kos_call would block forever. A bring-up refusal panics
    // instead. serve_loop returns only after an EPIPE, which means no client is parked.
    void xmcssc_service(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        // The line is claimed and delegated by start(), so it is already owned before
        // bring_up's CCR write arms RIEN/AIEN, which is the ordering the first receive
        // event needs.
        int const h = KOS_DRIVER_CAP_IRQ;

        if (not bring_up(win))
        {
            kos_panic("[xmcssc] channel bring-up refused (see the ERROR line above)");
        }

        UsicSscBus bus;
        bus.init(win, h);

        kickos::emit("[xmcssc] SPI service up (USIC0-CH1 SSC, IRQ-paced, HW CS on SELO0)\n");

        kickos::spi::serve_loop(bus);

        kos_exit(0);
    }

    int xmc_spi0_start(struct kos_service_cfg const* cfg)
    {
        if (cfg == nullptr or cfg->kind != KOS_SVC_SPI)
        {
            kos::print("[xmcssc] ERROR: bad or non-SPI service cfg\n");
            return -1;
        }

        uintptr_t const win_base = cfg->mmio_base;
        uint32_t const win_size = cfg->mmio_window;
        uint8_t const driver_prio = cfg->prio;

        // No register access here: root holds no DEV region at all (ARCH_MPU_DEV is
        // attached only by thread_spawn), so the driver thread is the only thread that can
        // address the channel. USIC0's module clock is already ungated by the console
        // (U0C0) bring-up.

        // 1. Create the request endpoint E (full rights: WAIT|SIGNAL|TRANSFER). Root KEEPS
        //    this cap so the app, on the same thread and table, can delegate a
        //    SIGNAL-narrowed copy to each client. g_spi0_ep records the handle.
        int const ep = kos_endpoint_create();
        if (ep < 0)
        {
            kos::print("[xmcssc] ERROR: endpoint_create failed\n");
            return -1;
        }

        // 2. Claim the USIC0 SR1 line HERE, not in the driver: the mint needs AUTH_IRQ
        //    and the driver runs at authority 0. The line comes back MASKED, so nothing
        //    can fire on it until the driver's first wait arms it. EDGE trigger: the
        //    receive flags are W1C'd by the driver before it acks.
        int const irq = kos_irq_claim(USIC0_SR1_IRQ, KOS_IRQ_EDGE);
        if (irq < 0)
        {
            kos::print("[xmcssc] ERROR: irq_claim(USIC0 SR1) failed\n");
            kos_handle_close(ep);
            return -1;
        }

        // 3. Spawn the UNPRIVILEGED driver: granted the U0C1 window (R|W|DEV), a WAIT-only
        //    recv cap on E (child index 1) and a WAIT-only copy of the line cap (index 2).
        //    No SIGNAL/TRANSFER on either child cap: the driver receives and services, it
        //    does not send, ring its own doorbell, or re-delegate.
        int const drv = kickos::driver::spawn_unprivileged(
            xmcssc_service, win_base, win_size, cfg->name, driver_prio, ep,
            "[xmcssc] ERROR: driver spawn failed\n", irq);
        if (drv < 0)
        {
            kos_handle_close(irq);
            return -1;
        }

        // Root drops its own copy: with the driver the only holder, the line comes back to
        // the pool when it dies.
        kos_handle_close(irq);
        g_spi0_ep = ep;
        return 0;
    }
}
