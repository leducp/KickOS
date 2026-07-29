// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 USIC "PV write only" probe. The XMC4700/XMC4800 Reference Manual (V1.3,
// 2016-07) Table 18-20 marks exactly three per-channel registers write-PV-only --
// FDR (0x010), BRG (0x014), CCR (0x040) -- while every other register this project's
// SPI bring-up touches (KSCFG 0x00C, INPR 0x018, DX0CR 0x01C, SCTR 0x034, TCSR 0x038,
// PCR 0x03C, PSCR 0x04C) is U,PV for both read and write. The question this app
// answers on silicon: when an UNPRIVILEGED thread holding an MPU grant for the channel
// window writes FDR/BRG/CCR, does the write land, get dropped, or fault?
// docs/design-unprivileged-root.md section 9 rests on the answer.
//
// The probe target is USIC0 channel 1 (0x4003_0200). U0C0 (0x4003_0000) is the console
// UART: garbling it destroys the only output channel at the bench.
//
// Two controls decide whether a negative result means anything:
//   * POSITIVE -- the same unprivileged thread writes SCTR (0x034, U,PV). If SCTR
//     lands, the grant and the MMIO path demonstrably work.
//   * NEGATIVE -- the thread ends on an UNGRANTED SCU poke. If that does not fault,
//     the MPU is not enforcing and the run says nothing about privilege.
//
// Each write is announced BEFORE it is issued, so a bus fault identifies which
// register faulted rather than losing the whole sequence.
//
// Register addresses / bit fields are clean-room from the Reference Manual; no
// XMCLib/DAVE/CMSIS vendor source.
//
// Diagnostic app (kickos_add_diagnostic_app): build-only here, validated by the
// operator on silicon.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <regs/usic.h> // shared XMC USIC register offsets + SSC bit fields

#include <stdint.h>

// Without enforcement the granted window is a no-op and the "unprivileged" thread is
// not confined, so every result below is meaningless.
#if !KICKOS_HAVE_MPU
#error "pvprobe requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

using namespace kickos::xmc::reg::usic;

namespace
{
    constexpr uint32_t U0C1_WINDOW = 0x200u;

    // Ungranted SCU clock-gate register (RM 11.*), the negative control.
    constexpr uintptr_t SCU_CGATCLR0 = 0x50004648u;

    // Pattern A (what the unprivileged thread writes) and pattern B (what root leaves
    // in place, so a dropped A is visible as an unchanged B).
    //
    // FDR: DM[15:14]=00B keeps the divider OFF so the read-only RESULT[25:16] field
    // cannot drift between the two reads; A and B differ only in STEP[9:0].
    constexpr uint32_t FDR_A = 0x00000155u;
    constexpr uint32_t FDR_B = 0x000002AAu;
    // BRG: A and B differ only in PDIV[25:16].
    constexpr uint32_t BRG_A = 0x01550000u;
    constexpr uint32_t BRG_B = 0x02AA0000u;
    // CCR: A selects SSC and arms the receive interrupts; B leaves MODE=0 (channel
    // disabled). Nothing is ever loaded into TBUF here, so SSC mode stays inert and
    // no signal is routed to a pin.
    constexpr uint32_t CCR_A = CCR_MODE_SSC | CCR_RIEN | CCR_AIEN;
    constexpr uint32_t CCR_B = CCR_TBIEN;
    // SCTR (the positive control): A and B differ in WLE[27:24], FLE[21:16] and SDIR.
    constexpr uint32_t SCTR_A = SCTR_WLE_8 | SCTR_FLE_8 | SCTR_TRM_ACTIVE | SCTR_SDIR_MSB;
    constexpr uint32_t SCTR_B = (3u << 24) | (3u << 16) | SCTR_TRM_ACTIVE;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    void show(char const* tag, char const* reg, uint32_t val)
    {
        char s[96];
        ksnprintf(s, sizeof(s), "[pvprobe] %s %s=0x%x\n", tag, reg,
                  static_cast<unsigned>(val));
        kos::print(s);
    }

    // Privileged write + read-back: the reference for what a write that DID land looks
    // like, reserved and read-only bits included.
    void root_write(char const* reg, uintptr_t addr, uint32_t val)
    {
        r32(addr) = val;
        uint32_t const got = r32(addr);
        char const* v = "exact";
        if (got != val)
        {
            v = "DIFF (reserved/read-only bits)";
        }
        char s[112];
        ksnprintf(s, sizeof(s), "[pvprobe] root %s: wrote=0x%x read=0x%x %s\n", reg,
                  static_cast<unsigned>(val), static_cast<unsigned>(got), v);
        kos::print(s);
    }

    // Unprivileged write + read-back. The "writing" line is emitted BEFORE the store so
    // a fault leaves it as the marker of which register faulted.
    void unpriv_write(char const* reg, uintptr_t addr, uint32_t val)
    {
        uint32_t const pre = r32(addr);
        char s[112];
        ksnprintf(s, sizeof(s), "[pvprobe] unpriv %s: pre=0x%x writing 0x%x ...\n", reg,
                  static_cast<unsigned>(pre), static_cast<unsigned>(val));
        kos::print(s);

        r32(addr) = val;

        uint32_t const post = r32(addr);
        char const* v = "DROPPED (post == pre)";
        if (post == val)
        {
            v = "LANDED (post == written)";
        }
        else if (post != pre)
        {
            v = "PARTIAL (post != pre, != written)";
        }
        ksnprintf(s, sizeof(s), "[pvprobe] unpriv %s: post=0x%x %s\n", reg,
                  static_cast<unsigned>(post), v);
        kos::print(s);
    }

    void probe(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        kos::print("[pvprobe] unprivileged probe up (granted U0C1 window 0x200)\n");
        show("unpriv inherited", "FDR", r32(win + off::FDR));
        show("unpriv inherited", "BRG", r32(win + off::BRG));
        show("unpriv inherited", "CCR", r32(win + off::CCR));
        show("unpriv inherited", "SCTR", r32(win + off::SCTR));

        unpriv_write("SCTR[U,PV control]", win + off::SCTR, SCTR_A);
        unpriv_write("FDR[PV]", win + off::FDR, FDR_A);
        unpriv_write("BRG[PV]", win + off::BRG, BRG_A);
        unpriv_write("CCR[PV]", win + off::CCR, CCR_A);

        kos::print("[pvprobe] poking UNGRANTED SCU @ 0x50004648 (expect MPU FAULT)\n");
        uint32_t const leaked = r32(SCU_CGATCLR0);

        char s[112];
        ksnprintf(s, sizeof(s),
                  "[pvprobe] UNGRANTED ACCESS DID NOT FAULT (SCU=0x%x): MPU not enforcing\n",
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
    kos::print("[pvprobe] XMC4800 U0C1 PV-write probe (RM V1.3 Table 18-20)\n");

    r32(U0C1_BASE + off::KSCFG) = KSCFG_MODEN | KSCFG_BPMODEN;
    // RM p.18-165: read KSCFG back before touching other USIC registers to flush the
    // control-block pipeline; the barrier keeps the volatile read from being elided.
    // With MODEN=0 the channel is inaccessible for read AND write except KSCFG, so a
    // dropped write measured before this point would be a confound, not a result.
    uint32_t const kscfg = r32(U0C1_BASE + off::KSCFG);
    __asm volatile("" : : "r"(kscfg) : "memory");
    show("root", "KSCFG", kscfg); // BPMODEN is a write-enable and reads back 0

    kos::print("[pvprobe] root baseline: pattern A\n");
    root_write("FDR", U0C1_BASE + off::FDR, FDR_A);
    root_write("BRG", U0C1_BASE + off::BRG, BRG_A);
    root_write("CCR", U0C1_BASE + off::CCR, CCR_A);
    root_write("SCTR", U0C1_BASE + off::SCTR, SCTR_A);

    kos::print("[pvprobe] root reset: pattern B\n");
    root_write("FDR", U0C1_BASE + off::FDR, FDR_B);
    root_write("BRG", U0C1_BASE + off::BRG, BRG_B);
    root_write("CCR", U0C1_BASE + off::CCR, CCR_B);
    root_write("SCTR", U0C1_BASE + off::SCTR, SCTR_B);

    int const p = kos::thread::spawn(probe, reinterpret_cast<void*>(U0C1_BASE),
                                     "pvprobe", 10, KOS_POLICY_FIFO, 0,
                                     /*privileged=*/false,
                                     /*mem=*/nullptr, /*mem_size=*/0,
                                     /*stack=*/nullptr, /*stack_size=*/0,
                                     /*mmio=*/reinterpret_cast<void*>(U0C1_BASE),
                                     U0C1_WINDOW);
    if (p < 0)
    {
        kos::print("[pvprobe] ERROR: probe spawn failed\n");
    }

    // Park: fall back to a sleep park if the semaphore could not be created (else a -1
    // handle spins a hot loop of failing sem_wait syscalls).
    int const idle = kos_sem_create(0);
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
