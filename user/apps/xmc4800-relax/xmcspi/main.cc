// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 USIC0-CH1 SSC (SPI) internal-loopback driver: the PMSA-proven M5
// board-support / userspace-library SPI reference, the XMC counterpart of the
// canonical F411 driver (user/apps/f411spi). On ARMv7-M the MPU is CPU-side and
// covers peripheral space, so a granted DEV window IS a genuine per-thread
// capability (reprogrammed every switch-in by arch_mpu_apply) -- unlike K64F,
// where peripherals are gated coarsely by the AIPS bridge (user/apps/k64dspi).
//
// A privileged bring-up shim enables the U0C1 kernel clock and configures the
// channel as an SSC master in internal LOOP-BACK mode (RM 18.2.3.5: the DX0
// input stage selects internal input "G" = the channel's own transmitter, so a
// byte shifts out DOUT0 and is received on DIN0 entirely on-chip -- NO port pins
// and NO external MISO<->MOSI jumper). It then spawns the UNPRIVILEGED driver
// granted ONLY the 512 B U0C1 channel window (0x4003_0200, DEV R|W no-X) + the
// USIC0 SR1 IRQ (NVIC 85, tier-1). The escalation surfaces -- the SCU clock tree
// and the port IOCR pin-mux -- stay OUT of the window; keeping them out is what
// makes the window a real capability. The driver runs a byte loopback (rx == tx
// per word), then pokes the UNGRANTED SCU clock-gate register which on PMSA MUST
// fault MemManage -- the per-thread peripheral-isolation result.
//
// USIC0 module clock + kernel channel U0C0 are already ungated by the console
// bring-up (kickos_xmc_usic_init, U0C0 = 0x4003_0000); this app leaves the SCU
// untouched so it is a clean ungranted target for the negative test.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800
// Reference Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. "RM
// p.NN" citations are the manual's printed page numbers.
//
// Diagnostic app (kickos_add_diagnostic_app): build-only, never a production
// image; the operator flashes + validates on silicon.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <regs/usic.h> // shared XMC USIC register offsets + SSC bit fields

#include <stdint.h>

// This app EXISTS to prove PMSA per-thread peripheral enforcement. Without it the
// MPU is a no-op, the ungranted poke below succeeds, and the console prints the
// isolation-FAILURE line -- a false "PMSA does not gate peripherals" verdict.
#if !KICKOS_HAVE_MPU
#error "xmcspi requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

// Register offsets (off::<REG>) and SSC bit fields are the shared chip definitions.
using namespace kickos::xmc::reg::usic;

namespace
{
    // USIC0 channel 1 register block: offsets + SSC bit fields come from the shared
    // chip header (regs/usic.h). The console owns U0C0 (0x4003_0000); SPI uses the
    // sibling channel U0C1 (U0C1_BASE = 0x4003_0200).

    // U0C1 channel window granted to the driver: base = channel base, size =
    // 0x200 (512 B), R|W|DEV no-X. PMSA-encodable: 512 is pow2 >= 32 B min and
    // 0x4003_0200 is 0x200-aligned -> one descriptor, exact-cover, no pad/split.
    // Every register the driver touches (CCR 0x40, PSR 0x48, PSCR 0x4C, RBUF
    // 0x54, TBUF0 0x80) lies inside 0x000..0x1FF.
    constexpr uint32_t U0C1_WINDOW = 0x200u;

    // PSR/PSCR [SSC Mode] (RM p.18-102 / p.18-171). A single-word frame is always
    // the FIRST word of its frame (RBUFSR.SOF=1), which sets the ALTERNATIVE
    // receive flag AIF, not RIF (RM 18.4.2.7). We arm + clear both to be robust:
    // RIF(14)/AIF(15) in PSR, CRIF(14)/CAIF(15) in PSCR (W1C). RX-complete implies
    // the word was shifted out AND in -> one wait covers full-duplex.
    constexpr uint32_t PSCR_CLEAR_RX = PSCR_CRIF | PSCR_CAIF;

    constexpr int USIC0_SR1_IRQ = 85; // RM Table 4-3

    // Ungranted peripheral for the negative test: SCU clock-gate clear register
    // (RM 11.*: SCU_CGATCLR0 = 0x5000_4648), the clock-tree escalation surface the
    // design keeps privileged + out-of-window. On PMSA an unprivileged access MUST
    // MemManage before any bus access.
    constexpr uintptr_t SCU_CGATCLR0 = 0x50004648u;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // UNPRIVILEGED driver: granted app code+data (auto), the U0C1 window (spawn
    // MMIO grant) and the USIC0 SR1 IRQ (tier-1). No file-scope mutable state under
    // enforcement: the window base arrives as the thread arg VALUE (never
    // dereferenced as memory), buffers live on the granted stack.
    void spi_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // U0C1 window base
        volatile uint32_t* pscr = reinterpret_cast<volatile uint32_t*>(win + off::PSCR);
        volatile uint32_t* rbuf = reinterpret_cast<volatile uint32_t*>(win + off::RBUF);
        volatile uint32_t* tbuf0 = reinterpret_cast<volatile uint32_t*>(win + off::TBUF0);

        int h = kos_irq_register(USIC0_SR1_IRQ);
        if (h < 0)
        {
            kos::print("[xmcspi] ERROR: irq_register(USIC0 SR1) failed\n");
            while (true)
            {
                kos_sleep_ns(1000000000ull);
            }
        }

        // The receive interrupt enables (CCR.RIEN/AIEN) were armed by the privileged
        // bring-up: CCR is a PV-write-only register at the bus (RM Table 18-20), and an
        // unprivileged write here is SILENTLY DISCARDED -- no fault, read-back unchanged
        // (measured by user/apps/xmc4800-relax/pvprobe). NVIC 85 is masked until
        // kos_irq_register above, so no event is lost by arming them at boot.

        // Announce before the first blocking wait: if SR1/NVIC 85 never fires
        // (misrouted node / RINP/AINP), the driver hangs in kos_irq_wait -- this
        // line disambiguates a hung-waiting-for-IRQ board from a dead one.
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

            *pscr = PSCR_CLEAR_RX; // W1C AIF/RIF BEFORE re-arm: an un-cleared level
                                   // re-asserts SR1 on unmask and storms it
                                   // (the timer-flag hazard).

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

        // Negative test (the canonical proof): touch the UNGRANTED SCU clock-gate
        // register. On PMSA the CPU-side MPU faults this BEFORE any bus access ->
        // MemManage, reported by the armv7m fault handler as "=== MPU FAULT ==="
        // with MMFAR=0x50004648. Announce-before-poke so the console shows intent
        // then the fault. Terminal, so it is the LAST thing we do.
        kos::print("[xmcspi] poking UNGRANTED SCU @ 0x50004648 (expect MPU FAULT)\n");
        uint32_t leaked = r32(SCU_CGATCLR0);

        // Only reached if PMSA did NOT enforce -- an isolation failure, not a pass.
        char s[72];
        ksnprintf(s, sizeof(s),
                  "[xmcspi] UNGRANTED ACCESS DID NOT FAULT (SCU=0x%x)\n",
                  static_cast<unsigned>(leaked));
        kos::print(s);
        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
    // Privileged bring-up (this main runs privileged): the one-time unsafe setup
    // the unprivileged driver must NOT be able to do. USIC0's module clock is
    // already ungated by the console (U0C0) bring-up; here only the U0C1 kernel
    // clock, baud, SSC master config and loopback input-mux are programmed. The
    // SCU and port IOCR stay privileged + out of the driver's window.
    r32(U0C1_BASE + off::KSCFG) = KSCFG_MODEN | KSCFG_BPMODEN;
    // RM p.18-165: read KSCFG back before touching other USIC registers to flush
    // the control-block pipeline; the barrier keeps the volatile read from being
    // elided.
    uint32_t kscfg_sync = r32(U0C1_BASE + off::KSCFG);
    __asm volatile("" : : "r"(kscfg_sync) : "memory");

    // Baud generator (fractional divider + SSC bit-time dividers).
    r32(U0C1_BASE + off::FDR) = FDR_DM_FRACTIONAL | FDR_STEP_367;
    r32(U0C1_BASE + off::BRG) = BRG_PDIV_13 | BRG_DCTQ_15 | BRG_PCTQ_0;

    // Shift + transmit + protocol config while the channel is still disabled
    // (CCR.MODE=0). SSC master, 8-bit MSB-first, single-shot start-on-TDV.
    r32(U0C1_BASE + off::SCTR) = SCTR_WLE_8 | SCTR_FLE_8 | SCTR_TRM_ACTIVE | SCTR_SDIR_MSB;
    r32(U0C1_BASE + off::TCSR) = TCSR_TDEN_TDV | TCSR_TDSSM;
    r32(U0C1_BASE + off::PCR) = PCR_MSLSEN;
    r32(U0C1_BASE + off::PSCR) = PSCR_CLEAR_RX; // clear stale RIF/AIF (defined bits only)

    // Internal loop-back: DX0 receives the channel's own transmitter (input "G")
    // routed to the data shift unit. Input-stage config must be done while
    // CCR.MODE=0 (RM p.18-57).
    r32(U0C1_BASE + off::DX0CR) = DX0CR_INSW | DX0CR_DSEL_G;

    // Route the receive / alternative-receive interrupts to service-request SR1
    // (NVIC 85). RIEN/AIEN are armed here too: CCR is PV-write-only at the bus (RM
    // Table 18-20), so the unprivileged driver cannot arm them itself.
    r32(U0C1_BASE + off::INPR) = INPR_RINP_SR1 | INPR_AINP_SR1;

    // Enable the channel + arm the receive interrupts in one PV write (config now
    // complete). NVIC 85 stays masked until the driver's kos_irq_register.
    r32(U0C1_BASE + off::CCR) = CCR_MODE_SSC | CCR_RIEN | CCR_AIEN;

    // No port pin-mux: loopback is internal (RM 18.2.3.5), so SCLK/MOSI/MISO are
    // never steered onto pins -- the IOCR escalation surface is not even touched.

    int drv = kos::thread::spawn(spi_driver, reinterpret_cast<void*>(U0C1_BASE),
                                 "xmcspi", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                 /*mem=*/nullptr, /*mem_size=*/0,
                                 /*stack=*/nullptr, /*stack_size=*/0,
                                 /*mmio=*/reinterpret_cast<void*>(U0C1_BASE), U0C1_WINDOW);
    if (drv < 0)
    {
        // Console is the only oracle at the bench: a silent dead board must not be
        // mistaken for a bring-up failure, so say so.
        kos::print("[xmcspi] ERROR: driver spawn failed\n");
    }

    // Park: fall back to a sleep park if the semaphore could not be created (else a
    // -1 handle spins a hot loop of failing sem_wait syscalls).
    int idle = kos_sem_create(0);
    while (true)
    {
        if (idle < 0)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
