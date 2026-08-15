// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 USIC0-CH1 SSC hardware chip-select HOLD bench: proves on silicon
// whether the MSLS/SELO chip-select stays asserted across a software-paced
// multi-word SPI frame, and that PCR.FEM (Frame End Mode, bit 3) is the bit that
// governs it. This is the gate for using a HARDWARE chip-select in the XMC SPI
// service: if the CS drops between software-paced words the slave sees N frames
// instead of one, so a held CS must be proven, not assumed.
//
// Modelled on user/apps/xmc4800-relax/xmcspi (same U0C1 = 0x4003_0200 512 B window, same
// unprivileged-driver-brings-itself-up + spawn-with-MMIO-grant pattern, with
// FDR/BRG/CCR going through kos_periph_reg_write). It is NOT the enforcement proof:
// no negative test, no MPU-fault poke; this bench answers only the functional CS-hold
// question.
//
// The finding under test (RM 18.4.5.1, PCR.FEM description, printed 18-99):
//   FEM = 0 (reset): "an end of frame is assumed if the transmit buffer TBUF does
//           not [have valid data] when the last bit of a data word has been sent
//           out" -> the frame ends the instant software is late with the next
//           word -> MSLS deactivates -> one CS pulse PER WORD.
//   FEM = 1: "The MSLS signal is kept active also while no new [data is
//           available] ... [no] automatic deactivation of MSLS in multi-word data
//           frames" -> MSLS stays asserted across the software gap -> ONE CS
//           bracket for the whole frame.
//
// The primary method is INTERNAL: MSLS is observed through the channel's own
// status register (PSR.MSLS level + PSR.MSLSEV edge flag), so NO port pin is
// routed (no IOCR mux) and NO jumper/scope is required. RX completion is a clean
// on-chip loopback (DX0 = own transmitter, RM 18.2.3.5), also pin-free.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800
// Reference Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. "RM p.NN"
// citations are the manual's printed page numbers.
//
// Diagnostic app (kickos_add_diagnostic_app): build-only, never a production
// image; the operator flashes + validates on silicon.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <regs/usic.h> // shared XMC USIC register offsets + SSC bit fields

#include <stdint.h>

// Mirrors xmcspi: the driver runs unprivileged in a granted DEV window, which is
// only a real capability under PMSA. Gate on enforcement so the window means
// something (and so the build config matches the xmcspi reference).
#if !KICKOS_HAVE_MPU
#error "xmccshold requires enforcement: build the board's base variant, not its flat one"
#endif

// Register offsets (off::<REG>) and SSC bit fields are the shared chip definitions.
using namespace kickos::xmc::reg::usic;

namespace
{
    // USIC0 channel 1 register block: offsets + SSC bit fields come from the shared
    // chip header (regs/usic.h). The console owns U0C0 (0x4003_0000); this bench uses
    // the sibling channel U0C1 (U0C1_BASE = 0x4003_0200).

    // U0C1 window granted to the driver: base = channel base, size = 0x200
    // (512 B), R|W|DEV no-X. Every register the driver touches (SCTR 0x34, TCSR
    // 0x38, PCR 0x3C, PSR 0x48, PSCR 0x4C, RBUF 0x54, TBUF0 0x80) lies inside.
    constexpr uint32_t U0C1_WINDOW = 0x200u;

    // TCSR frame-control base (start-on-TDV + single-shot); SOF/EOF are added per word.
    constexpr uint32_t TCSR_BASE = TCSR_TDEN_TDV | TCSR_TDSSM;

    // PCR SSC-master base: internal MSLS, direct active-low SELO0. FEM (the frame-end
    // mode bit under test) is OR-ed in at the write site.
    constexpr uint32_t PCR_SSC_BASE = PCR_MSLSEN | PCR_SELCTR_DIRECT | PCR_SELINV_LOW | PCR_SELO0;

    // The FIRST word of a frame completes into AIF, later words into RIF, so
    // (RIF|AIF) covers every word (RM 18.4.2.7).
    constexpr uint32_t PSR_RX_DONE = PSR_RIF | PSR_AIF;

    // Clear both RX-complete flags (a single-word frame lands in AIF).
    constexpr uint32_t PSCR_CLEAR_RX = PSCR_CRIF | PSCR_CAIF;

    // Frame shape: 4 words, MSB pattern immaterial (data is discarded; only the CS
    // behaviour matters).
    constexpr unsigned FRAME_WORDS = 4u;
    constexpr uint8_t FRAME_PATTERN[FRAME_WORDS] = {0xA5u, 0x3Cu, 0x00u, 0xFFu};

    // Deliberate slow-software pacing between words. Long enough that a real driver
    // could not "sneak" the next word in before the SSC engine sees TBUF empty.
    // This is the exact condition FEM must survive.
    constexpr uint64_t WORD_GAP_NS = 3000000ull; // 3 ms

    // Bounded polls (RM/house rule: never an unbounded MMIO poll). After each TBUF
    // write we spin-poll PSR: CAPTURE_MAX is the hard ceiling so a dead channel
    // cannot wedge the bench; POST_RX_MARGIN is a short tail spun AFTER RX
    // completion so the trailing MSLS fall (FEM=0, and the FEM=1 end-of-frame) is
    // caught in the same capture window, before the WORD_GAP sleep, keeping every
    // edge attributed to exactly one word (no cross-word coalescing).
    constexpr unsigned CAPTURE_MAX = 500000u;
    constexpr unsigned POST_RX_MARGIN = 4000u;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // Drive one 4-word frame over the granted window and return the number of MSLS
    // transitions observed. FEM is already programmed in PCR by the caller; this
    // routine only writes the SOF/EOF markers, the TBUF words, and counts edges.
    unsigned run_frame(uintptr_t win)
    {
        volatile uint32_t* tcsr = reinterpret_cast<volatile uint32_t*>(win + off::TCSR);
        volatile uint32_t* psr = reinterpret_cast<volatile uint32_t*>(win + off::PSR);
        volatile uint32_t* pscr = reinterpret_cast<volatile uint32_t*>(win + off::PSCR);
        volatile uint32_t* rbuf = reinterpret_cast<volatile uint32_t*>(win + off::RBUF);
        volatile uint32_t* tbuf0 = reinterpret_cast<volatile uint32_t*>(win + off::TBUF0);

        // Start clean: clear any stale MSLS level, event, and RX flags so the edge
        // count reflects only this frame.
        *pscr = PSCR_MSLS | PSCR_MSLSEV | PSCR_CLEAR_RX;

        unsigned edges = 0;
        for (unsigned i = 0; i < FRAME_WORDS; i++)
        {
            // Frame markers must be set BEFORE the TBUF write (they are sampled when
            // TDV goes valid): SOF on word 0, EOF on the last word.
            uint32_t tcsr_val = TCSR_BASE;
            if (i == 0)
            {
                tcsr_val = tcsr_val | TCSR_SOF;
            }
            if (i == (FRAME_WORDS - 1))
            {
                tcsr_val = tcsr_val | TCSR_EOF;
            }
            *tcsr = tcsr_val;

            *tbuf0 = FRAME_PATTERN[i]; // sets TDV -> master clocks one 8-bit word

            // Tight capture window: count every MSLS transition and release RX.
            unsigned spins = 0;
            bool rx_done = false;
            unsigned post = 0;
            while (spins < CAPTURE_MAX)
            {
                uint32_t s = *psr;
                if ((s & PSR_MSLSEV) != 0u)
                {
                    edges++;
                    *pscr = PSCR_MSLSEV; // W1C so the next transition is a fresh edge
                }
                if ((s & PSR_RX_DONE) != 0u and not rx_done)
                {
                    rx_done = true;
                    uint32_t rx = *rbuf; // read releases the receive buffer
                    static_cast<void>(rx);
                    *pscr = PSCR_CLEAR_RX;
                }
                if (rx_done)
                {
                    post++;
                    if (post >= POST_RX_MARGIN)
                    {
                        break;
                    }
                }
                spins++;
            }

            // Deliberate slow-software gap. FEM=1: MSLS stays asserted (no edge).
            // FEM=0: MSLS already fell inside the capture window (no edge here).
            kos_sleep_ns(WORD_GAP_NS);
        }

        // Stop any still-running frame (defensive): W1C PSR.MSLS deactivates MSLS.
        *pscr = PSCR_MSLS;
        return edges;
    }

    // UNPRIVILEGED driver: granted app code+data (auto) and the U0C1 window (spawn
    // MMIO grant). No IRQ is registered; the bench polls. No file-scope mutable
    // state: the window base arrives as the thread arg VALUE, counters are locals.
    constexpr uint32_t FDR_WORD = FDR_DM_FRACTIONAL | FDR_STEP_367;
    constexpr uint32_t BRG_WORD = BRG_PDIV_13 | BRG_DCTQ_15 | BRG_PCTQ_0;

    // FDR.RESULT[25:16] is driven by the fractional divider and excluded from the
    // read-back comparison.
    constexpr uint32_t FDR_RESULT_MASK = 0x03FF0000u;

    // One PV-classified register through the seam, then read back: a discarded store is
    // silent at the bus, and the errno separates a refused call from a dropped one.
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
        ksnprintf(s, sizeof(s), "[xmccshold] seam %s: rc=%d wrote=0x%x read=0x%x %s\n",
                  reg, rc, static_cast<unsigned>(val), static_cast<unsigned>(got),
                  verdict);
        kos::print(s);
        return ok;
    }

    // Bring-up, run by the thread that holds the window. Run 1 is fully configured under
    // CCR.MODE=0 (RM 18.4.3 ordering rule); PCR starts with FEM=1.
    bool bring_up(uintptr_t win)
    {
        r32(win + off::KSCFG) = KSCFG_MODEN | KSCFG_BPMODEN;
        // RM p.18-165: read KSCFG back before touching other USIC registers to flush the
        // control-block pipeline; keep the volatile read from being elided.
        uint32_t const kscfg_sync = r32(win + off::KSCFG);
        __asm volatile("" : : "r"(kscfg_sync) : "memory");

        bool ok = seam_write("FDR", win, off::FDR, FDR_WORD, ~FDR_RESULT_MASK);
        ok = seam_write("BRG", win, off::BRG, BRG_WORD, 0xFFFFFFFFu) and ok;

        // SSC master, 8-bit MSB-first, single-shot start-on-TDV, FLE=63 so software
        // SOF/EOF govern the frame end. All U,PV: direct stores.
        r32(win + off::SCTR) = SCTR_WLE_8 | SCTR_FLE_63 | SCTR_TRM_ACTIVE | SCTR_SDIR_MSB;
        r32(win + off::TCSR) = TCSR_BASE;
        r32(win + off::PCR) = PCR_SSC_BASE | PCR_FEM;
        r32(win + off::PSCR) = PSCR_MSLS | PSCR_MSLSEV | PSCR_CLEAR_RX; // clear stale

        // Internal loop-back: DX0 receives the channel's own transmitter (input "G"),
        // giving a clean per-word RX-complete marker with no port pin. Input-stage config
        // must be done while CCR.MODE=0 (RM p.18-57).
        r32(win + off::DX0CR) = DX0CR_INSW | DX0CR_DSEL_G;

        // Enable the channel (config complete). No RX interrupt enables: the bench polls
        // PSR. No INPR / NVIC routing, no IOCR pin-mux: MSLS is observed internally, so
        // this is a fully pin-free measurement.
        ok = seam_write("CCR", win, off::CCR, CCR_MODE_SSC, 0xFFFFFFFFu) and ok;
        return ok;
    }

    void cs_hold_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);
        volatile uint32_t* pcr = reinterpret_cast<volatile uint32_t*>(win + off::PCR);

        if (not bring_up(win))
        {
            kos_panic("[xmccshold] bring-up FAILURE: a PV register did not take the seam write");
        }

        // Run 1: FEM=1 (PCR carries FEM from the bring-up above).
        // Announce before the polling frame so a wedged board is diagnosable.
        kos::print("[xmccshold] run 1 FEM=1: driving 4-word software-paced frame\n");
        unsigned fem1_edges = run_frame(win);

        char s[96];
        char const* v1 = "FAIL";
        if (fem1_edges == 2u)
        {
            v1 = "PASS";
        }
        ksnprintf(s, sizeof(s), "[xmccshold] FEM=1: MSLS edges = %u : %s\n",
                  fem1_edges, v1);
        kos::print(s);

        // Reconfigure for run 2: flip FEM to 0 (PCR is U-writable, RM Table 18-20).
        // The channel is idle (frame ended, MSLS forced inactive above), and this is
        // a same-protocol parameter change, so no CCR MODE cycle is needed.
        *pcr = PCR_SSC_BASE; // FEM bit cleared

        kos::print("[xmccshold] run 2 FEM=0: driving 4-word software-paced frame\n");
        unsigned fem0_edges = run_frame(win);

        char const* v0 = "FAIL";
        // FEM=0 pulses MSLS once per word: rise+fall x 4 words = 8 edges. Accept a
        // window around 8 (edge capture can under-count by one on a very tight
        // trailing delay); the discriminator is "clearly more than the held case".
        if (fem0_edges >= 6u and fem0_edges <= 10u)
        {
            v0 = "PASS";
        }
        ksnprintf(s, sizeof(s), "[xmccshold] FEM=0: MSLS edges = %u : %s\n",
                  fem0_edges, v0);
        kos::print(s);

        // Verdict: the HW CS-hold is usable ONLY if the bit provably governs:
        // FEM=1 holds (2 edges = one bracket) AND FEM=0 pulses (~8 = per-word). A
        // board that simply always holds (2 and 2) or always pulses would fail here.
        char const* verdict = "hardware CS-hold NOT usable (FEM does not govern)";
        if (fem1_edges == 2u and (fem0_edges >= 6u and fem0_edges <= 10u))
        {
            verdict = "hardware CS-hold USABLE (FEM=1 holds, FEM=0 pulses)";
        }
        ksnprintf(s, sizeof(s), "[xmccshold] VERDICT: %s\n", verdict);
        kos::print(s);

        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
    // No register access from root: the bring-up belongs to the thread that holds the
    // window. USIC0's module clock is already ungated by the console (U0C0) bring-up.
    // The driver flips only PCR.FEM for run 2, on the idle channel.

    auto drv = kos::thread::spawn(cs_hold_driver, reinterpret_cast<void*>(U0C1_BASE),
                                  "xmccshold", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                  /*mem=*/nullptr, /*mem_size=*/0,
                                  /*stack=*/nullptr, /*stack_size=*/0,
                                  /*mmio=*/reinterpret_cast<void*>(U0C1_BASE), U0C1_WINDOW);
    if (not drv.valid())
    {
        // -KOS_EBUSY: a live domain already holds U0C1, which this bench needs
        // exclusively, so no service list carrying an SSC/SPI entry may run alongside
        // it. The kernel console path drops every byte once a driver has published, so
        // the errno goes out through the panic path.
        char e[64];
        ksnprintf(e, sizeof(e), "[xmccshold] U0C1 spawn refused, errno %d", -drv.error());
        kos_panic(e);
    }

    kos_cap_t idle = KOS_CAP_NONE;

    (void)kos_sem_create(0, &idle);
    while (true)
    {
        if (idle == KOS_CAP_NONE)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
