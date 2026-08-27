// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 USIC0-CH1 SSC (SPI) internal-loopback driver. On ARMv7-M the MPU is CPU-side
// and covers peripheral space, so a granted DEV window IS a genuine per-thread
// capability (reprogrammed every switch-in by arch_mpu_apply).
//
// The UNPRIVILEGED driver is granted ONLY the 512 B U0C1 channel window (0x4003_0200,
// DEV R|W no-X) and brings the channel up itself; root writes no register at all and
// holds no DEV region. FDR/BRG/CCR go through kos_periph_reg_write because those three
// are write-PV-only at the bus. LOOP-BACK is internal (RM 18.2.3.5): the DX0 input stage
// selects internal input "G", the channel's own transmitter, so a byte shifts out DOUT0
// and back in on DIN0 entirely on-chip, with NO port pins and NO external MISO<->MOSI
// jumper. The escalation surfaces (the SCU clock tree and the port IOCR pin-mux) stay
// OUT of the window; keeping them out is what makes the window a real capability, and
// the final poke at the UNGRANTED SCU clock-gate register MUST fault MemManage.
//
// USIC0's module clock and kernel channel U0C0 are already ungated by the console
// bring-up (kickos_xmc_usic_init, U0C0 = 0x4003_0000); this app must leave the SCU
// untouched so it stays a clean ungranted target for the negative test.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800
// Reference Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. "RM
// p.NN" citations are the manual's printed page numbers.
//
// Diagnostic app (kickos_add_diagnostic_app): build-only, never a production image.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <regs/usic.h> // shared XMC USIC register offsets + SSC bit fields

#include <stdint.h>

// Without enforcement the MPU is a no-op, the ungranted poke below succeeds and the
// console prints the isolation-FAILURE line: a vacuous test reporting a false "PMSA
// does not gate peripherals" verdict.
#if !KICKOS_HAVE_MPU
#error "xmcspi requires enforcement: build the board's base variant, not its flat one"
#endif

using namespace kickos::xmc::reg::usic;

namespace
{
    // The console owns U0C0 (0x4003_0000), so SPI must use the sibling channel U0C1
    // (U0C1_BASE = 0x4003_0200).

    // PMSA-encodable in one exact-cover descriptor: 512 is pow2 >= the 32 B minimum and
    // 0x4003_0200 is 0x200-aligned, so no pad or split. Every register the driver touches
    // lies inside the window, TBUF0 (0x080) being the highest.
    constexpr uint32_t U0C1_WINDOW = 0x200u;

    // A single-word frame is the FIRST word of its frame (RBUFSR.SOF=1), so it raises
    // the ALTERNATIVE receive flag AIF, not RIF (RM 18.4.2.7; PSR/PSCR at RM p.18-102 /
    // p.18-171): both must be armed and both cleared. RX-complete implies the word was
    // shifted out as well as in, so one wait covers full duplex.
    constexpr uint32_t PSCR_CLEAR_RX = PSCR_CRIF | PSCR_CAIF;

    constexpr int USIC0_SR1_IRQ = 85; // RM Table 4-3

    // Negative-test target (RM 11.*: SCU_CGATCLR0 = 0x5000_4648): a clock-tree
    // escalation surface that must stay out of every granted window. On PMSA an
    // unprivileged access MUST MemManage before any bus access.
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
        kos::print(s);
        return ok;
    }

    // The RM's ordering is mandatory: KSCFG first (with MODEN=0 nothing but KSCFG is
    // reachable), every configuration register while CCR.MODE=0, CCR last.
    bool bring_up(uintptr_t win)
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

        // Route the receive / alternative-receive interrupts to service-request SR1
        // (NVIC 85). INPR is U,PV.
        r32(win + off::INPR) = INPR_RINP_SR1 | INPR_AINP_SR1;

        ok = seam_write("CCR", win, off::CCR, CCR_WORD, 0xFFFFFFFFu) and ok;
        return ok;
    }

    // Unprivileged, granted only app code+data, the U0C1 window and a WAIT-only cap on
    // the USIC0 SR1 line. It must touch no file-scope mutable state under enforcement:
    // the window base arrives as the thread arg VALUE (never dereferenced as memory)
    // and buffers live on the granted stack.
    void spi_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // U0C1 window base
        volatile uint32_t* pscr = reinterpret_cast<volatile uint32_t*>(win + off::PSCR);
        volatile uint32_t* rbuf = reinterpret_cast<volatile uint32_t*>(win + off::RBUF);
        volatile uint32_t* tbuf0 = reinterpret_cast<volatile uint32_t*>(win + off::TBUF0);

        int const h = KOS_SPAWN_DELEGATED_CAP0; // the only delegated cap: the line

        // The line must be owned before bring_up's last act arms CCR.RIEN/AIEN and the
        // first receive event can fire; root claimed it before this thread existed.
        if (not bring_up(win))
        {
            kos_panic("[xmcspi] bring-up FAILURE: a PV register did not take the seam write");
        }

        // Must print before the first blocking wait: if SR1/NVIC 85 never fires
        // (misrouted node / RINP/AINP) the driver hangs in kos_irq_wait, and without
        // this line a board hung on the IRQ looks like a dead one.
        kos::print("[xmcspi] starting SSC loopback (blocking on USIC0 SR1 IRQ 85)\n");

        // Known pattern; each byte round-trips through the on-chip loopback equal.
        uint8_t const pattern[] = {0xA5u, 0x3Cu, 0x00u, 0xFFu};
        int fails = 0;
        for (unsigned i = 0; i < sizeof(pattern); i++)
        {
            uint32_t tx = pattern[i];

            *tbuf0 = tx; // load TX buffer -> TDV=1 -> master clocks one 8-bit frame

            kos_irq_wait(h);             // block until AIF/RIF raises NVIC 85
            uint32_t rx = *rbuf & 0xFFu; // read RX: releases the standard buffer

            *pscr = PSCR_CLEAR_RX; // W1C AIF/RIF BEFORE the ack, so the flag is already
                                   // clear when kos_irq_ack unmasks the line.

            char s[64];
            char const* verdict = "PASS";
            if (rx != tx)
            {
                verdict = "FAIL";
                fails++;
            }
            ksnprintf(s, sizeof(s), "[xmcspi] word %u: tx=0x%x rx=0x%x %s\n",
                      i, static_cast<unsigned>(tx), static_cast<unsigned>(rx), verdict);
            kos::print(s);

            kos_irq_ack(h); // unmask NVIC 85 (flag already clear -> no storm)
        }

        if (fails == 0)
        {
            kos::print("[xmcspi] loopback PASS (all words echoed equal)\n");
        }
        else
        {
            kos::print("[xmcspi] loopback FAIL (word mismatch)\n");
        }

        // Negative test: on PMSA this ungranted access faults BEFORE any bus access.
        // armv7m opted into fault isolation, so this KILLS the thread
        // ("=== THREAD FAULT === thread 'xmcspi' killed", ADDR=0x50004648) rather than
        // panicking. Terminal for this thread, so it must stay the LAST thing it does,
        // and the announce must precede the poke or the console shows only the fault.
        kos::print("[xmcspi] poking UNGRANTED SCU @ 0x50004648 (expect MPU FAULT)\n");
        uint32_t leaked = r32(SCU_CGATCLR0);

        // Reached only if PMSA did NOT enforce: an isolation failure, not a pass.
        char s[72];
        ksnprintf(s, sizeof(s),
                  "[xmcspi] UNGRANTED ACCESS DID NOT FAULT (SCU=0x%x)\n",
                  static_cast<unsigned>(leaked));
        kos::print(s);
        kos_panic("[xmcspi] isolation FAILURE: ungranted read landed");
    }
}

// KOS_AUTH_IRQ: the line mint is namespace-wide, so root claims the line and hands the
// driver a cap. The driver itself runs at authority 0.
KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_IRQ);

int main(int, char**)
{
    // Deliberately no register access and no pin-mux here: the bring-up belongs to the
    // window holder, and the loopback is internal (RM 18.2.3.5), so SCLK/MOSI/MISO must
    // never be steered onto pins.

    // EDGE: the driver W1Cs PSCR before it acks, so a bare unmask cannot storm.
    kos_cap_t irq = KOS_CAP_NONE;
    int const irq_rc = kos_irq_claim(USIC0_SR1_IRQ, KOS_IRQ_EDGE, &irq);
    if (irq_rc != 0)
    {
        char e[64];
        ksnprintf(e, sizeof(e), "[xmcspi] irq_claim(85) refused, errno %d", -irq_rc);
        kos_panic(e);
    }
    kos_cap_grant const caps[1] = {{irq, KOS_CAP_WAIT}};

    // The driver ends on the negative test's fault, and a fault cancels the faulting
    // thread's whole TASK: spawned plain it would join root's task and take root with it,
    // leaving no survivor to keep the board up. Root holds the handle for the life of the
    // image, since it never reaches a point past the driver.
    kos_task_t victim = KOS_TASK_NONE;
    if (kos_task_create(nullptr, 0, 0, &victim) != 0)
    {
        kos_panic("[xmcspi] no task slot for the driver");
    }

    auto drv = kos::thread::create(spi_driver, reinterpret_cast<void*>(U0C1_BASE),
                                   "xmcspi", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                   /*mem=*/nullptr, /*mem_size=*/0,
                                   /*stack=*/nullptr, /*stack_size=*/0,
                                   /*mmio=*/reinterpret_cast<void*>(U0C1_BASE), U0C1_WINDOW,
                                   caps, 1, /*authority=*/0, /*cap_dest=*/nullptr, victim);
    if (not drv.valid())
    {
        // -KOS_EBUSY: a live domain already holds U0C1, which this app needs exclusively,
        // so no service list carrying an SSC/SPI entry may run alongside it. The errno goes
        // out through the panic path because the kernel console path drops every byte once a
        // driver has published.
        char e[64];
        ksnprintf(e, sizeof(e), "[xmcspi] U0C1 driver spawn refused, errno %d", -drv.error());
        kos_panic(e);
    }
    // Root's copy must go, else the line stays pinned by a cap nobody waits on instead
    // of returning to the pool when the driver dies.
    kos_handle_close(irq);

    // Sleep park when the semaphore could not be created: an unmintable handle would spin
    // a hot loop of failing sem_wait syscalls.
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
