// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800/USIC0-CH1 SSC (SPI) bus SERVICE (see <kickos/driver/xmcssc.h>). The XMC
// sibling of user/driver/k64dspi; three parts, part-for-part the same shape:
//
//   1. UsicSscBus -- the transaction ENGINE (a class, no globals, no IPC). Given the
//      granted U0C1 window it folds each device's config into SCTR (word/bit-order) +
//      PCR (the SSC-master CS framing) words, rewrites them from the named device's
//      profile at the head of every transfer, and clocks N bytes one word at a time,
//      blocking on the USIC0 SR1 RX-complete IRQ per word (rx overwrites tx in
//      place). For KOS_BUS_CS_HW the hardware MSLS/SELO0 line brackets the WHOLE
//      transfer: PCR.FEM=1 holds MSLS asserted across the software gaps between
//      words (RM 18.4.5.1), TCSR.SOF marks word 0 and TCSR.EOF the last, so the
//      slave sees one coherent frame (the xmccshold finding).
//   2. xmcssc_service -- the unprivileged driver thread: kos_recv a kos_bus_req,
//      validate it, run the class transaction over the concatenated segment bytes,
//      kos_reply a kos_bus_rsp. The reply cap is consumed on EVERY loop path.
//   3. xmc_spi0_start -- the one-time bring-up (run from root) + endpoint + spawn.
//
// The data path is INTERNAL LOOP-BACK (DX0 = own transmitter, input "G", RM
// 18.2.3.5): no external device on the bench, so rx == tx on-chip and SELO0 is armed
// but never routed to a pin (the IOCR pin-mux stays privileged + untouched). So this
// service proves the call/reply + bus ABI + CS-hold framing plumbing; the MSLS hold
// itself is proven on silicon by user/apps/xmccshold.
//
// Register split: the bring-up sets the kernel clock, baud profile (FDR/BRG), input
// routing (INPR) and channel enable (CCR) once; the driver thread folds SCTR/TCSR/PCR
// and runs the transfer. Everything sits inside the granted 0x200 window, but the split
// is NOT merely placement: FDR/BRG/CCR are write-PV-only at the bus, and an
// unprivileged write to them is silently discarded (no fault) -- measured on silicon by
// user/apps/xmc4800-relax/pvprobe, docs/design-unprivileged-root.md section 9. So this
// bring-up needs a privileged executor. CPOL/CPHA (BRG.SCLKCFG) are fixed to
// SPI mode 0 at bring-up; a device profile therefore carries only bit order + word
// size + the CS framing -- no per-device rate and no per-device CPOL/CPHA on this
// chip (the internal loopback is phase-agnostic).
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
#include <kickos/io/mmio.h>            // r32

#include <regs/usic.h> // shared XMC USIC register offsets + SSC bit fields

#include <stdint.h>
#include <stddef.h>

// Register offsets (off::<REG>) and SSC bit fields are the shared chip definitions.
using namespace kickos::xmc::reg::usic;

namespace
{
    // --- USIC0-CH1 per-channel register offsets + SSC bit fields come from the
    // shared chip header (regs/usic.h). The console owns U0C0 (0x4003_0000); SPI
    // uses the sibling channel U0C1 (0x4003_0200), whose base arrives as the service
    // cfg's mmio_base. ---

    // Fixed baud profile (reused verbatim from xmcspi): the FDR/BRG register values
    // are in the shared header (FDR_STEP_367 / BRG_*); these mirror the same profile
    // as plain integers for the nominal-rate report (profile_bitclock below).
    constexpr uint32_t BAUD_STEP = 367u;
    constexpr uint32_t BAUD_PDIV_PLUS1 = 14u;
    constexpr uint32_t BAUD_DCTQ_PLUS1 = 16u;

    // SCTR field positions for the client-configurable word size (FLE/WLE are N+1).
    constexpr unsigned SCTR_FLE_SHIFT = 16u;
    constexpr unsigned SCTR_WLE_SHIFT = 24u;

    // TCSR frame-control base (start-on-TDV + single-shot); SOF/EOF added per word.
    constexpr uint32_t TCSR_BASE = TCSR_TDEN_TDV | TCSR_TDSSM;

    // PCR hardware-CS base: SSC master, direct active-low SELO0, plus FEM to hold the
    // CS across a multi-word frame (RM 18.4.5.1, proven by xmccshold).
    constexpr uint32_t PCR_CS_HW = PCR_MSLSEN | PCR_SELCTR_DIRECT | PCR_SELINV_LOW | PCR_SELO0 | PCR_FEM;

    // Clear both RX-complete flags (a single-word frame lands in AIF, RM 18.4.2.7).
    constexpr uint32_t PSCR_CLEAR_RX = PSCR_CRIF | PSCR_CAIF;

    constexpr int USIC0_SR1_IRQ = 85; // RM Table 4-3

    // Fallback peripheral clock if the branch-clock oracle does not know USIC0 (0
    // return): XMC4800 peripheral bus clock at 144 MHz. The oracle is authoritative
    // when it answers. Used ONLY to report the nominal profile rate; the profile
    // dividers are fixed regardless.
    constexpr uint32_t USIC_CLK_FALLBACK = 144000000u;

    // Nominal SSC bit clock of the fixed bring-up profile off `f_periph`: fractional
    // divider output fFD = f_periph * STEP/1024, then /(PDIV+1)/2/(DCTQ+1). Truthful
    // for the fixed profile (informational over the loopback; the client cannot
    // retune it -- configure() never refolds FDR/BRG).
    uint32_t profile_bitclock(uint32_t f_periph)
    {
        uint64_t f_fd = (static_cast<uint64_t>(f_periph) * BAUD_STEP) / 1024u;
        uint64_t rate = f_fd / BAUD_PDIV_PLUS1 / 2u / BAUD_DCTQ_PLUS1;
        return static_cast<uint32_t>(rate);
    }

    // The transaction ENGINE. No IPC, no policy beyond the CS framing the coherent
    // transfer demands. All register access is INSIDE the granted U0C1 window (a real
    // per-thread capability on PMSA). The per-word RX-complete is the USIC0 SR1 IRQ.
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
        // DISCARDED -- FDR/BRG are write-PV-only, the bus drops the store here -- and
        // CPOL/CPHA (BRG.SCLKCFG) are fixed to mode 0 at bring-up for the same reason.
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
                out.pcr = PCR_CS_HW; // MSLSEN|SELCTR|SELINV|SELO0|FEM (the CS-hold recipe)
            }

            uint32_t f = kos_periph_clock_hz(win_);
            if (f == 0u)
            {
                f = USIC_CLK_FALLBACK;
            }
            return profile_bitclock(f);
        }

        // Apply device `p`'s profile to the live SCTR/PCR, then clock `len` bytes one
        // word at a time; tx bytes in `buf` go out, rx overwrites them in place. Each
        // word: set SOF/EOF (CS_HW), load TBUF0 (starts one frame), block on the SR1
        // RX-complete IRQ, drain RBUF, clear the flags. For CS_HW the MSLS/SELO0 line
        // asserts on word 0 (SOF) and, held by FEM across the software gaps, releases
        // only after the last word (EOF) -- one coherent transaction spanning ALL
        // segments of the message.
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
}

extern "C"
{
    int xmc_spi0_take_endpoint(void)
    {
        int const ep = g_spi0_ep;
        g_spi0_ep = -1; // one-shot: device slots are caller-named, so ONE client only
        return ep;
    }

    // UNPRIVILEGED driver thread: owns the U0C1 window (spawn MMIO grant) + the USIC0
    // SR1 IRQ (tier-1, registered here). The window base arrives as the arg VALUE
    // (never dereferenced as memory). The delegated recv cap lands at child index 1.
    void xmcssc_service(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        int const h = kos_irq_register(USIC0_SR1_IRQ);
        if (h < 0)
        {
            kos::print("[xmcssc] ERROR: irq_register(USIC0 SR1) failed\n");
            while (true)
            {
                kos_sleep_ns(1000000000ull);
            }
        }

        UsicSscBus bus;
        bus.init(win, h);

        kos::print("[xmcssc] SPI service up (USIC0-CH1 SSC, IRQ-paced, HW CS on SELO0)\n");

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
        bool const cs_hw = (cfg->cs_policy == KOS_SVC_CS_HW);

        // 1. One-time bring-up, run from the root/init thread: kernel clock gate,
        //    baud (FDR/BRG), INPR routing, CCR enable. FDR/BRG/CCR are write-PV-only
        //    at the bus, so this must run privileged: an unprivileged store to them is
        //    silently discarded (docs/design-unprivileged-root.md section 9).
        //    USIC0's module clock is already ungated by the console (U0C0)
        //    bring-up. The SCU clock tree and the port IOCR pin-mux stay out of the
        //    driver's window (the escalation surfaces the capability keeps out).
        r32(win_base + off::KSCFG) = KSCFG_MODEN | KSCFG_BPMODEN;
        // RM p.18-165: read KSCFG back before touching other USIC registers to flush
        // the control-block pipeline; keep the volatile read from being elided.
        uint32_t kscfg_sync = r32(win_base + off::KSCFG);
        __asm volatile("" : : "r"(kscfg_sync) : "memory");

        // Baud generator (fractional divider + SSC bit-time dividers): set once here,
        // fixed for the channel's life.
        r32(win_base + off::FDR) = FDR_DM_FRACTIONAL | FDR_STEP_367;
        r32(win_base + off::BRG) = BRG_PDIV_13 | BRG_DCTQ_15 | BRG_PCTQ_0;

        // Initial shift + transmit + protocol config while the channel is disabled
        // (CCR.MODE=0): the channel must be coherently configured before CCR enables it.
        // The driver rewrites SCTR/PCR from the named device's profile inside every
        // transfer, and refuses an XFER on a slot that has had no CONFIG, so this seed
        // is never what a transfer runs on.
        uint32_t sctr = SCTR_TRM_ACTIVE | (7u << SCTR_WLE_SHIFT) | SCTR_SDIR_MSB; // 8-bit MSB
        uint32_t pcr = PCR_MSLSEN;
        if (cs_hw)
        {
            sctr |= SCTR_FLE_63;
            pcr = PCR_CS_HW;
        }
        else
        {
            sctr |= (7u << SCTR_FLE_SHIFT); // one 8-bit word per frame
        }
        r32(win_base + off::SCTR) = sctr;
        r32(win_base + off::TCSR) = TCSR_BASE;
        r32(win_base + off::PCR) = pcr;
        r32(win_base + off::PSCR) = PSCR_MSLS | PSCR_CLEAR_RX; // clear stale flags

        // Internal loop-back: DX0 receives the channel's own transmitter (input "G")
        // routed to the data shift unit. Input-stage config must be done while
        // CCR.MODE=0 (RM p.18-57). No external device on the bench -> rx == tx.
        r32(win_base + off::DX0CR) = DX0CR_INSW | DX0CR_DSEL_G;

        // Route the receive / alternative-receive interrupts to service-request SR1
        // (NVIC 85). NVIC 85 stays masked until the driver's kos_irq_register, so no
        // event is lost by arming at boot.
        r32(win_base + off::INPR) = INPR_RINP_SR1 | INPR_AINP_SR1;

        // Enable the channel + arm the receive interrupts in one CCR write (config
        // now complete).
        r32(win_base + off::CCR) = CCR_MODE_SSC | CCR_RIEN | CCR_AIEN;

        // No IOCR pin-mux: the data path is internal loopback and SELO0 is never routed
        // to a pin, so the IOCR escalation surface is not even touched.

        // 2. Create the request endpoint E (full rights: WAIT|SIGNAL|TRANSFER). Root
        //    KEEPS this cap so the app -- same thread, same table -- can delegate a
        //    SIGNAL-narrowed copy to each client. g_spi0_ep records the handle.
        int const ep = kos_endpoint_create();
        if (ep < 0)
        {
            kos::print("[xmcssc] ERROR: endpoint_create failed\n");
            return -1;
        }

        // 3. Spawn the UNPRIVILEGED driver: granted the U0C1 window (R|W|DEV -- a real
        //    per-thread capability on PMSA, reprogrammed every switch-in) and a WAIT-only
        //    recv cap on E (child index 1). No SIGNAL/TRANSFER on the child cap: the
        //    driver receives, it does not send or re-delegate. The USIC0 SR1 IRQ is
        //    registered by the driver itself (the tier-1 substrate), not a spawn cap.
        int const drv = kickos::driver::spawn_unprivileged(
            xmcssc_service, win_base, win_size, cfg->name, driver_prio, ep,
            "[xmcssc] ERROR: driver spawn failed\n");
        if (drv < 0)
        {
            return -1;
        }

        g_spi0_ep = ep;
        return 0;
    }
}
