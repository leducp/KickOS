// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 cross-channel interrupt-injection probe. An unprivileged thread holding a
// legitimate MPU grant for USIC0 channel 1 (0x4003_0200, size 0x200) reroutes that
// channel's receive interrupt onto the kernel console's service-request node and
// storms it, to test whether it can deny service to the console.
//
// The wiring the attack exploits:
//   * the kernel console TX drains on U0C0 -> USIC0 SR0 -> NVIC 84, bound as an in-kernel
//     handler (console_tx.cc console_tx_isr);
//   * INPR (channel-window offset 0x018) selects the SRx node for RIF/AIF, and RINP/AINP
//     field 0 selects SR0, the console's node. INPR is U,PV and in-window;
//   * DX0CR internal loopback makes every TBUF0 write produce a receive event, and the
//     attacker arms CCR.RIEN|AIEN itself through kos_periph_reg_write, whose only
//     credential is the U0C1 grant it holds.
// The whole sequence needs nothing from root, which makes the finding a property of the
// grant rather than of the bring-up split.
//
// MEASURED ON SILICON (XMC4800-Relax, 2026-07-28), AT ONE OPERATING POINT: the reroute
// lands (INPR 0x1100 -> 0x0) and the injection is real. With the kernel console_tx_isr
// instrumented the foreign events drove it at ~37,700 invocations/second, the console
// KEPT BEATING and the board did NOT wedge. A separate run left the RIF/AIF flag set and
// the attacker idle: console_tx_isr took ZERO extra entries, so the XMC USIC receive
// service request is EDGE (one pulse per received word), not the sustained level the DoS
// premise assumes. A held, uncleared flag does not re-assert SR0 and console_tx_isr
// always returns.
//
// THAT RATE IS NOT A CEILING. The event rate is one pulse per shifted word, and FDR/BRG
// (the baud divider chain) are two of the three allowlist entries the seam accepts from
// the window holder, so the attacker sets the rate itself. Build with
// -DKICKOS_INPRSTORM_MAX_RATE=ON (target inprstormmax) to drive every divider to its
// minimum; the default target keeps the 115.2 kHz-profile comparison point. What each
// profile PINS is the programmed divider fields, their read-back and the branch clock the
// kernel reports; the shift-clock frequency itself is read off the wire, since the RM's
// fSCLK factor for SSC master is unconfirmed here.
//
// Third profile (INPRSTORM_FIFO, target inprstormfifo) removes the per-word CPU cost: the
// TX FIFO drains words back-to-back. RM V1.3 Table 18-20 marks TBCTR (0x108), the TBUFx
// aperture (0x080..0x0FC) and TRBSR (0x114) all U,PV, so the window holder arms the FIFO
// with no seam entry; only FDR/BRG/CCR are write-PV. The FIFO auto-loads TBUF whenever
// TCSR.TDV=0 (RM 18.2.8.4), so the shift clock, not the attacker's CPU share, sets the
// receive-event rate. This profile forces the max-rate divider chain, and the two effects
// multiply.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference
// Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. Diagnostic app
// (kickos_add_diagnostic_app): never a production image.
//
// BUILD WITH THE KERNEL-CONSOLE SERVICE LIST (KICKOS_SERVICE_LIST=kickos_services_none):
// the kernel console_tx path is the target, and the xmcuart handover deinits it.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <regs/usic.h> // shared XMC USIC register offsets + SSC bit fields

#include <stdint.h>

#if !KICKOS_HAVE_MPU
#error "inprstorm requires enforcement: build the board's base variant, not its flat one"
#endif

// Set by the inprstormmax target; the default target keeps the comparison profile.
#ifndef INPRSTORM_MAX_RATE
#define INPRSTORM_MAX_RATE 0
#endif
// Set by the inprstormfifo target; it arms the TX FIFO and forces the max-rate divider.
#ifndef INPRSTORM_FIFO
#define INPRSTORM_FIFO 0
#endif

#if INPRSTORM_FIFO
#define INPRSTORM_USE_MAX_RATE 1
#else
#define INPRSTORM_USE_MAX_RATE INPRSTORM_MAX_RATE
#endif

using namespace kickos::xmc::reg::usic;

namespace
{
    constexpr uint32_t U0C1_WINDOW = 0x200u;

    // Three profiles over two distinct divider chains: the FIFO profile reuses the max-rate
    // one. FDR fractional mode gives fFD = fPERIPH*STEP/1024
    // (RM p.18-178); BRG divides it by (PDIV+1)*(PCTQ+1)*(DCTQ+1) with CTQSEL=0 selecting
    // fPDIV as the time-quantum source (RM p.18-179). STEP is FDR[9:0]; the BRG fields sit
    // at the shifts in regs/usic.h.
#if INPRSTORM_FIFO
    constexpr char const* PROFILE_NAME = "FIFO autonomous drain + MAX RATE";
    constexpr uint32_t P_STEP = FDR_STEP_MASK; // 1023/1024, the largest fractional divider
    constexpr uint32_t P_PDIV = 0u;            // PDIV+1 = 1
    constexpr uint32_t P_PCTQ = 0u;            // PCTQ+1 = 1
    constexpr uint32_t P_DCTQ = 0u;            // DCTQ+1 = 1
#elif INPRSTORM_MAX_RATE
    constexpr char const* PROFILE_NAME = "MAX RATE";
    constexpr uint32_t P_STEP = FDR_STEP_MASK; // 1023/1024, the largest fractional divider
    constexpr uint32_t P_PDIV = 0u;            // PDIV+1 = 1
    constexpr uint32_t P_PCTQ = 0u;            // PCTQ+1 = 1
    constexpr uint32_t P_DCTQ = 0u;            // DCTQ+1 = 1
#else
    constexpr char const* PROFILE_NAME = "115.2 kHz comparison";
    constexpr uint32_t P_STEP = FDR_STEP_367;
    constexpr uint32_t P_PDIV = 13u; // PDIV+1 = 14
    constexpr uint32_t P_PCTQ = 0u;  // PCTQ+1 = 1
    constexpr uint32_t P_DCTQ = 15u; // DCTQ+1 = 16
#endif

    // TX FIFO arming registers, all U,PV in RM V1.3 Table 18-20 (in-window, no seam).
    constexpr uint32_t CCFG_TB = 1u << 7;                     // TX FIFO present (RM p.18-164)
    constexpr uint32_t TBCTR_SIZE_MASK = 0x7u << FIFO_SIZE_SHIFT;
    constexpr uint32_t TBCTR_SIZE_64 = 6u << FIFO_SIZE_SHIFT; // 64 entries (RM p.18-216)
    constexpr uint32_t TRBSR_TFULL = 1u << 12;                // TX FIFO full (RM p.18-209)
    constexpr uint32_t TRBSR_TBFLVL_SHIFT = 24;               // TBFLVL[30:24] (RM p.18-209)
    // FIFO push aperture: a write to INx (0x180+x*4) stores into the transmit FIFO
    // (RM p.18-223). A write to TBUF0 (0x080) is the STANDARD buffer and bypasses the
    // FIFO, so the FIFO storm must feed IN0, not TBUF0.
    constexpr uintptr_t IN0 = 0x180u;
    constexpr uint32_t FDR_WORD = FDR_DM_FRACTIONAL | P_STEP;
    constexpr uint32_t BRG_WORD = (P_PDIV << BRG_PDIV_SHIFT) | (P_PCTQ << BRG_PCTQ_SHIFT)
                                | (P_DCTQ << BRG_DCTQ_SHIFT);

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    void show(char const* tag, uint32_t v)
    {
        char s[80];
        ksnprintf(s, sizeof(s), "[inprstorm] %s=0x%x\n", tag, static_cast<unsigned>(v));
        kos::print(s);
    }

    // One PV-classified register through the seam, then read back: a discarded store is
    // silent at the bus, and the errno separates a refused call from a dropped one.
    // FDR.RESULT[25:16] is driven by the fractional divider, so `care` excludes it.
    constexpr uint32_t FDR_RESULT_MASK = 0x03FF0000u;

    bool seam_write(char const* reg, uintptr_t win, uintptr_t off_reg, uint32_t val,
                    uint32_t care)
    {
        int const rc = kos_periph_reg_write(win, off_reg, val);
        uint32_t const got = r32(win + off_reg);
        bool const ok = (rc == 0) and ((got & care) == (val & care));
        char const* verdict = "DISCARDED/REFUSED";
        if (ok)
        {
            verdict = "LANDED";
        }
        char s[112];
        ksnprintf(s, sizeof(s), "[inprstorm] seam %s: rc=%d wrote=0x%x read=0x%x %s\n",
                  reg, rc, static_cast<unsigned>(val), static_cast<unsigned>(got),
                  verdict);
        kos::print(s);
        return ok;
    }

    void storm(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        kos::print("[inprstorm] unpriv up (granted U0C1 window 0x200)\n");

        // Printed before the storm runs: the divider chain the seam let the holder program,
        // and the branch clock the kernel reports for this block (0 when the chip does not
        // know it).
        {
            char s[128];
            ksnprintf(s, sizeof(s),
                      "[inprstorm] profile %s: STEP=%u PDIV+1=%u PCTQ+1=%u DCTQ+1=%u"
                      " fPERIPH=%u Hz\n",
                      PROFILE_NAME, static_cast<unsigned>(P_STEP),
                      static_cast<unsigned>(P_PDIV + 1u), static_cast<unsigned>(P_PCTQ + 1u),
                      static_cast<unsigned>(P_DCTQ + 1u),
                      static_cast<unsigned>(kos_periph_clock_hz(win)));
            kos::print(s);
        }

        // KSCFG first: until MODEN is set the channel answers nothing else. INPR starts on
        // the benign SR1 (NVIC 85, masked, no driver), so nothing storms until the reroute
        // below.
        r32(win + off::KSCFG) = KSCFG_MODEN | KSCFG_BPMODEN;
        uint32_t const kscfg = r32(win + off::KSCFG);
        __asm volatile("" : : "r"(kscfg) : "memory");

        r32(win + off::SCTR) = SCTR_TRM_ACTIVE | SCTR_WLE_8 | SCTR_FLE_8 | SCTR_SDIR_MSB;
        r32(win + off::TCSR) = TCSR_TDEN_TDV | TCSR_TDSSM;
        r32(win + off::PCR) = PCR_MSLSEN;
        r32(win + off::DX0CR) = DX0CR_INSW | DX0CR_DSEL_G; // internal loopback
        r32(win + off::PSCR) = PSCR_CRIF | PSCR_CAIF;      // clear stale RX flags
        r32(win + off::INPR) = INPR_RINP_SR1 | INPR_AINP_SR1;

        bool ok = seam_write("FDR", win, off::FDR, FDR_WORD, ~FDR_RESULT_MASK);
        ok = seam_write("BRG", win, off::BRG, BRG_WORD, 0xFFFFFFFFu) and ok;
        ok = seam_write("CCR", win, off::CCR, CCR_MODE_SSC | CCR_RIEN | CCR_AIEN,
                        0xFFFFFFFFu) and ok;
        if (not ok)
        {
            kos_panic("[inprstorm] bring-up FAILURE: a PV register did not take the seam write");
        }

#if INPRSTORM_FIFO
        // Arm the TX FIFO with only U,PV stores (no seam). TBCTR is writable only while
        // CCFG.TB=1 (RM p.18-214); CCFG.TB is a hardware capability bit.
        uint32_t const ccfg = r32(win + off::CCFG);
        show("CCFG (TB=bit7)", ccfg);
        if ((ccfg & CCFG_TB) == 0u)
        {
            kos_panic("[inprstorm] CCFG.TB=0: TX FIFO absent, vector unavailable");
        }
        // DPTR must be written while SIZE=0 (RM 18.2.8.1); DPTR=0, so write 0 then SIZE.
        r32(win + off::TBCTR) = 0u;
        r32(win + off::TBCTR) = TBCTR_SIZE_64;
        {
            uint32_t const tbctr = r32(win + off::TBCTR);
            char const* verdict = "DROPPED (TBCTR write ignored)";
            if ((tbctr & TBCTR_SIZE_MASK) == TBCTR_SIZE_64)
            {
                verdict = "LANDED (FIFO armed, no seam)";
            }
            char s[112];
            ksnprintf(s, sizeof(s), "[inprstorm] TBCTR=0x%x SIZE=64 %s\n",
                      static_cast<unsigned>(tbctr), verdict);
            kos::print(s);
        }
        // The backlog proof needs fill to outrun drain, so it runs at the slow 115.2k
        // divider: at the profile's max divider the drain outpaces any fill and the FIFO
        // stays empty (measured). Fill to TFULL, then take NO further action; a falling
        // TBFLVL is the channel clocking the backlog out with zero attacker CPU
        // (RM 18.2.8.4). INPR still points at the benign SR1, so the drained words raise
        // no console interrupt.
        {
            seam_write("FDR demo-slow", win, off::FDR, FDR_DM_FRACTIONAL | FDR_STEP_367,
                       ~FDR_RESULT_MASK);
            seam_write("BRG demo-slow", win, off::BRG,
                       BRG_PDIV_13 | BRG_DCTQ_15 | BRG_PCTQ_0, 0xFFFFFFFFu);
            volatile uint32_t* fill = reinterpret_cast<volatile uint32_t*>(win + IN0);
            uint32_t guard = 0u;
            while (((r32(win + off::TRBSR) & TRBSR_TFULL) == 0u) and (guard < 256u))
            {
                *fill = 0xA5u;
                guard++;
            }
            uint32_t const lvl_pre = (r32(win + off::TRBSR) >> TRBSR_TBFLVL_SHIFT) & 0x7Fu;
            kos_sleep_ns(10000000ull); // no fills during this window
            uint32_t const lvl_post = (r32(win + off::TRBSR) >> TRBSR_TBFLVL_SHIFT) & 0x7Fu;
            char s[128];
            ksnprintf(s, sizeof(s),
                      "[inprstorm] slow-divider preload TBFLVL=%u, after 10ms no-fill=%u"
                      " (backlog drains autonomously if falling)\n",
                      static_cast<unsigned>(lvl_pre), static_cast<unsigned>(lvl_post));
            kos::print(s);
        }
        // Restore the profile's max-rate divider for the worst-case sustained storm.
        seam_write("FDR", win, off::FDR, FDR_WORD, ~FDR_RESULT_MASK);
        seam_write("BRG", win, off::BRG, BRG_WORD, 0xFFFFFFFFu);
#endif

        // Root prints several heartbeats that DO drain first: the console is demonstrably
        // alive right up to the storm.
        kos_sleep_ns(2000000000ull);

        // The attack: RINP/AINP field 0 routes the receive interrupt to SR0. INPR is U,PV
        // and in-window, so this unprivileged store lands.
        kos::print("[inprstorm] rerouting INPR RINP/AINP -> SR0 (console node)\n");
        show("INPR before", r32(win + off::INPR));
        r32(win + off::INPR) = 0u;
        show("INPR after ", r32(win + off::INPR));
        show("CCR        ", r32(win + off::CCR));

#if INPRSTORM_FIFO
        // FIFO drain storm: keep the FIFO topped up. Between refills the channel shifts
        // the queued words out on its own, pulsing SR0 per word with no attacker CPU, so
        // the receive-event rate follows the shift clock, not this thread's CPU share.
        // RIF/AIF are never cleared (edge, one pulse per received word).
        kos::print("[inprstorm] FIFO drain storm: refilling TX FIFO, channel clocks SR0 autonomously\n");
        volatile uint32_t* in0 = reinterpret_cast<volatile uint32_t*>(win + IN0);
        while (true)
        {
            while ((r32(win + off::TRBSR) & TRBSR_TFULL) == 0u)
            {
                *in0 = 0xA5u;
            }
        }
#else
        // Continuous receive-event storm: keep clocking loopback words as fast as the
        // channel accepts them and NEVER clear RIF/AIF. Every completed word pulses SR0.
        kos::print("[inprstorm] storming TBUF0 forever (RIF/AIF never cleared) ...\n");
        volatile uint32_t* tbuf0 = reinterpret_cast<volatile uint32_t*>(win + off::TBUF0);
        volatile uint32_t* tcsr = reinterpret_cast<volatile uint32_t*>(win + off::TCSR);
        volatile uint32_t* rbuf = reinterpret_cast<volatile uint32_t*>(win + off::RBUF);
        while (true)
        {
            if ((r32(win + off::TCSR) & TCSR_TDV) == 0u)
            {
                *tcsr = TCSR_TDEN_TDV | TCSR_TDSSM;
                *tbuf0 = 0xA5u;
            }
            (void)*rbuf; // release RBUF so a full buffer does not block the next word
        }
#endif
    }
}

int main(int, char**)
{
    kos::print("[inprstorm] XMC4800 console DoS probe via U0C1 INPR reroute onto SR0\n");
    kos::print("[inprstorm] MARKER: root up, spawning the U0C1 holder\n");
    {
        char s[80];
        ksnprintf(s, sizeof(s), "[inprstorm] rate profile: %s\n", PROFILE_NAME);
        kos::print(s);
    }

    // No register access from root: the attacker configures U0C1 itself, so the whole
    // sequence is what a grant holder can reach.

    // Priority 1 (KICKOS_PRIO_MIN) is BELOW root's KICKOS_PRIO_MIN+1: the storm thread
    // can never starve root by hogging the CPU, so a wedged console would isolate the
    // foreign-SR0 interrupt storm as the cause.
    auto const p = kos::thread::spawn(storm, reinterpret_cast<void*>(U0C1_BASE),
                                      "inprstorm", 1, KOS_POLICY_FIFO, 0,
                                      /*privileged=*/false,
                                      /*mem=*/nullptr, /*mem_size=*/0,
                                      /*stack=*/nullptr, /*stack_size=*/0,
                                      /*mmio=*/reinterpret_cast<void*>(U0C1_BASE),
                                      U0C1_WINDOW);
    if (not p.valid())
    {
        // -KOS_EBUSY: a live domain already holds U0C1. Without the grant the attacker
        // never reaches INPR, and the heartbeat below would read as "no DoS" on a probe that
        // never fired. The errno goes out through the panic path because the kernel console
        // path drops every byte once a driver has published.
        char e[64];
        ksnprintf(e, sizeof(e), "[inprstorm] U0C1 spawn refused, errno %d", -p.error());
        kos_panic(e);
    }

    // Heartbeat. A DoS kills the log outright; a rate the console merely SURVIVES shows up
    // as dt drifting above the 300 ms nominal, which a bare beat counter cannot show. t is
    // uptime and dt the interval since the previous beat, both ms from the monotonic clock.
    uint64_t const t0 = kos_clock_now();
    uint64_t prev = t0;
    uint32_t beat = 0;
    while (true)
    {
        uint64_t const now = kos_clock_now();
        char s[80];
        ksnprintf(s, sizeof(s), "[inprstorm] heartbeat %u t=%ums dt=%ums\n",
                  static_cast<unsigned>(beat),
                  static_cast<unsigned>((now - t0) / 1000000ull),
                  static_cast<unsigned>((now - prev) / 1000000ull));
        kos::print(s);
        prev = now;
        beat++;
        kos_sleep_ns(300000000ull);
    }
}
