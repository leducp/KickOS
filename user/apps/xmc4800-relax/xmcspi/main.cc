// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 USIC0 channel 1 SSC internal loopback, run by the task's entry over the channel window
// and the line its composition grants. FDR, BRG and CCR take only privileged writes, so they go
// through kos_periph_reg_write. DX0 input G loops the transmitter back (RM 18.2.3.5), so no pin and
// no jumper is involved. The last act reads SCU_CGATCLR0, which nothing grants, and must fault.
// Register definitions are clean-room from the XMC4700/XMC4800 RM V1.3 (2016-07), printed pages.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>

#include <regs/usic.h> // shared XMC USIC register offsets + SSC bit fields

#include <stdint.h>
#include <stdlib.h>

// Without enforcement the ungranted read lands and prints an isolation-failure line.
#if !KICKOS_HAVE_MPU
#error "xmcspi requires enforcement: build the board's base variant, not its flat one"
#endif

using namespace kickos::xmc::reg::usic;

namespace
{
    constexpr uint32_t WINDOW_BYTES = 0x200u;

    // A single-word frame is the FIRST word of its frame (RBUFSR.SOF=1), so it raises
    // the ALTERNATIVE receive flag AIF, not RIF (RM 18.4.2.7; PSR/PSCR at RM p.18-102 /
    // p.18-171): both must be armed and both cleared. RX-complete implies the word was
    // shifted out as well as in, so one wait covers full duplex.
    constexpr uint32_t PSCR_CLEAR_RX = PSCR_CRIF | PSCR_CAIF;

    // A clock-tree escalation surface (RM 11.*) no window may hold: on PMSA an unprivileged
    // access faults before any bus access.
    constexpr uintptr_t SCU_CGATCLR0 = 0x50004648u;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    constexpr uint32_t FDR_WORD = FDR_DM_FRACTIONAL | FDR_STEP_367;
    constexpr uint32_t BRG_WORD = BRG_PDIV_13 | BRG_DCTQ_15 | BRG_PCTQ_0;
    constexpr uint32_t CCR_WORD = CCR_MODE_SSC | CCR_RIEN | CCR_AIEN;

    // FDR.RESULT[25:16] is driven by the fractional divider, so it is excluded from the
    // read-back comparison; every other bit of the three words reads back verbatim.
    constexpr uint32_t FDR_RESULT_MASK = 0x03FF0000u;

    // A discarded store is silent at the bus, so the read-back is the only evidence that
    // a PV write landed; the errno separates a refused call from a dropped one.
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
        ksnprintf(s, sizeof(s), "[xmcspi] seam %s: rc=%d wrote=0x%x read=0x%x %s\n", reg,
                  rc, static_cast<unsigned>(val), static_cast<unsigned>(got), verdict);
        kickos::emit(s);
        return ok;
    }

    // The RM's ordering is mandatory: KSCFG first (with MODEN=0 nothing but KSCFG is
    // reachable), every configuration register while CCR.MODE=0, CCR last. `node` is the line's
    // index within USIC0, the service request the receive events are routed to.
    bool bring_up(uintptr_t win, uint32_t node)
    {
        r32(win + off::KSCFG) = KSCFG_MODEN | KSCFG_BPMODEN;
        // RM p.18-165: read KSCFG back before touching other USIC registers to flush
        // the control-block pipeline; the barrier keeps the read from being elided.
        uint32_t const kscfg_sync = r32(win + off::KSCFG);
        __asm volatile("" : : "r"(kscfg_sync) : "memory");

        bool ok = seam_write("FDR", win, off::FDR, FDR_WORD, ~FDR_RESULT_MASK);
        ok = seam_write("BRG", win, off::BRG, BRG_WORD, 0xFFFFFFFFu) and ok;

        // Only valid while the channel is still disabled (CCR.MODE=0). These registers
        // are U,PV at the bus, so direct stores land.
        r32(win + off::SCTR) = SCTR_WLE_8 | SCTR_FLE_8 | SCTR_TRM_ACTIVE | SCTR_SDIR_MSB;
        r32(win + off::TCSR) = TCSR_TDEN_TDV | TCSR_TDSSM;
        r32(win + off::PCR) = PCR_MSLSEN;
        r32(win + off::PSCR) = PSCR_CLEAR_RX; // clear stale RIF/AIF (defined bits only)

        // DX0 receives the channel's own transmitter (input "G"). Input-stage config is
        // only accepted while CCR.MODE=0 (RM p.18-57).
        r32(win + off::DX0CR) = DX0CR_INSW | DX0CR_DSEL_G;

        // INPR is U,PV.
        r32(win + off::INPR) = (node << INPR_RINP_SHIFT) | (node << INPR_AINP_SHIFT);

        ok = seam_write("CCR", win, off::CCR, CCR_WORD, 0xFFFFFFFFu) and ok;
        return ok;
    }
}

extern "C" void xmcspi_main(kos_self_t const* self)
{
    kos_window_t const window = kos_grant_mmio(self, "/dev/usic0/ch1");
    kos_line_t const line = kos_grant_irq(self, "irq");
    uintptr_t const win = reinterpret_cast<uintptr_t>(kos_window_addr(window));
    if (win == 0u or kos_window_size(window) < WINDOW_BYTES or line.cap == KOS_CAP_NONE
        or line.index > INPR_SR_LAST)
    {
        kickos::emit("[xmcspi] ERROR: no /dev/usic0/ch1 window or irq line\n");
        exit(1);
    }
    volatile uint32_t* pscr = reinterpret_cast<volatile uint32_t*>(win + off::PSCR);
    volatile uint32_t* rbuf = reinterpret_cast<volatile uint32_t*>(win + off::RBUF);
    volatile uint32_t* tbuf0 = reinterpret_cast<volatile uint32_t*>(win + off::TBUF0);

    kos_cap_t note = KOS_CAP_NONE;
    int rc = kos_notify_create(&note);
    if (rc == 0)
    {
        rc = kos_notify_bind(note);
    }
    if (rc == 0)
    {
        rc = kos_irq_bind_notify(line.cap, note);
    }
    if (rc != 0)
    {
        char e[64];
        ksnprintf(e, sizeof(e), "[xmcspi] ERROR: the line's notification rc %d\n", rc);
        kickos::emit(e);
        exit(1);
    }

    // The init claimed the line before this task ran, so it is owned before CCR arms the
    // receive events below.
    if (not bring_up(win, line.index))
    {
        kickos::emit("[xmcspi] ERROR: bring-up: a PV register did not take the seam write\n");
        exit(1);
    }

    // Before the first wait: a misrouted node hangs the wait, and this line tells that apart from
    // a dead board.
    kickos::emit("[xmcspi] starting SSC loopback (blocking on the USIC0 line)\n");

    uint8_t const pattern[] = {0xA5u, 0x3Cu, 0x00u, 0xFFu};
    int fails = 0;
    for (unsigned i = 0; i < sizeof(pattern); i++)
    {
        uint32_t const tx = pattern[i];
        *tbuf0 = tx;

        (void)kos_notify_wait(note, 1u, KOS_TIMEOUT_NONE, nullptr);
        uint32_t const rx = *rbuf & 0xFFu;
        // Cleared before the next wait rearms the edge-claimed line, so it cannot storm.
        *pscr = PSCR_CLEAR_RX;

        char s[64];
        char const* verdict = "PASS";
        if (rx != tx)
        {
            verdict = "FAIL";
            fails++;
        }
        ksnprintf(s, sizeof(s), "[xmcspi] word %u: tx=0x%x rx=0x%x %s\n", i,
                  static_cast<unsigned>(tx), static_cast<unsigned>(rx), verdict);
        kickos::emit(s);
    }
    if (fails == 0)
    {
        kickos::emit("[xmcspi] loopback PASS (all words echoed equal)\n");
    }
    else
    {
        kickos::emit("[xmcspi] loopback FAIL (word mismatch)\n");
    }

    // Terminal: the announce must precede the read, or the console shows only the fault.
    kickos::emit("[xmcspi] poking UNGRANTED SCU @ 0x50004648 (expect MPU FAULT)\n");
    uint32_t const leaked = r32(SCU_CGATCLR0);

    char s[72];
    ksnprintf(s, sizeof(s), "[xmcspi] UNGRANTED ACCESS DID NOT FAULT (SCU=0x%x)\n",
              static_cast<unsigned>(leaked));
    kickos::emit(s);
    exit(1);
}
