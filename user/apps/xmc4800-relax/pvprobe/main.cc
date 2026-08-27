// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 USIC "PV write only" probe, and the gate on the privileged-write seam that
// answers it. The XMC4700/XMC4800 Reference Manual (V1.3, 2016-07) Table 18-20 marks
// exactly three per-channel registers write-PV-only, FDR (0x010), BRG (0x014) and CCR
// (0x040), while every other register this project's SPI bring-up touches (KSCFG
// 0x00C, INPR 0x018, DX0CR 0x01C, SCTR 0x034, TCSR 0x038, PCR 0x03C, PSCR 0x04C) is
// U,PV for both read and write.
//
// Everything runs in ONE unprivileged thread holding the channel grant, so the results are
// directly comparable. Two controls carry the run:
//   * SCTR (U,PV) written directly must LAND. That POSITIVE control proves the grant and
//     the MMIO path work, so a dropped store is about privilege;
//   * an UNGRANTED SCU poke must MemManage. Without that NEGATIVE control the MPU might
//     not be enforcing and the run would say nothing.
// The baseline value each direct store is compared against is itself installed through the
// seam, so a "DROPPED" verdict cannot come from writing what was already there.
//
// The probe target is USIC0 channel 1 (0x4003_0200). U0C0 (0x4003_0000) is the console
// UART, and garbling it destroys the only output channel at the bench.
//
// Register addresses / bit fields are clean-room from the Reference Manual; no
// XMCLib/DAVE/CMSIS vendor source. Diagnostic app (kickos_add_diagnostic_app): never a
// production image.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <regs/usic.h> // shared XMC USIC register offsets + SSC bit fields

#include <stdint.h>

// Without enforcement the granted window is a no-op and the "unprivileged" thread is
// not confined, so every result below is meaningless.
#if !KICKOS_HAVE_MPU
#error "pvprobe requires enforcement: build the board's base variant, not its flat one"
#endif

using namespace kickos::xmc::reg::usic;

namespace
{
    constexpr uint32_t U0C1_WINDOW = 0x200u;

    // Ungranted SCU clock-gate register (RM 11.*), the negative control.
    constexpr uintptr_t SCU_CGATCLR0 = 0x50004648u;

    // Pattern A is what the direct unprivileged store attempts; pattern B is the baseline
    // the seam installs first, so a dropped A is visible as an unchanged B.
    //
    // FDR: DM[15:14]=00B keeps the divider OFF so the read-only RESULT[25:16] field
    // cannot drift between the two reads; A and B differ only in STEP[9:0].
    constexpr uint32_t FDR_A = 0x00000155u;
    constexpr uint32_t FDR_B = 0x000002AAu;
    // BRG: A and B differ only in PDIV[25:16].
    constexpr uint32_t BRG_A = 0x01550000u;
    constexpr uint32_t BRG_B = 0x02AA0000u;
    // CCR: A selects SSC and arms both receive interrupts; B leaves MODE=0 (channel
    // disabled) and arms RIEN only, so A and B differ in MODE[3:0] and AIEN(15).
    // Nothing is ever loaded into TBUF here, so SSC mode stays inert and no signal is
    // routed to a pin; with MODE=0 pattern B can raise no receive event at all.
    //
    // Every pattern above lies inside the seam's per-entry value mask, which the seam
    // refuses whole rather than silently trims. CCR's mask is MODE[3:0]|RIEN|AIEN, which
    // leaves CCR_TBIEN for the out-of-mask probe below.
    constexpr uint32_t CCR_A = CCR_MODE_SSC | CCR_RIEN | CCR_AIEN;
    constexpr uint32_t CCR_B = CCR_RIEN;
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

    // Write through the privileged-write seam + read back: the reference for what a write
    // that DID land looks like, reserved and read-only bits included. The errno is reported
    // so a refusal (-KOS_EPERM lost grant, -KOS_EINVAL off the allowlist, -KOS_ENOSYS no
    // backend) is never read as a discarded store.
    void seam_write(char const* reg, uintptr_t win, uintptr_t off_reg, uint32_t val)
    {
        int const rc = kos_periph_reg_write(win, off_reg, val);
        uint32_t const got = r32(win + off_reg);
        char const* v = "DIFF (reserved/read-only bits)";
        if (got == val)
        {
            v = "exact";
        }
        char s[120];
        ksnprintf(s, sizeof(s), "[pvprobe] seam %s: rc=%d wrote=0x%x read=0x%x %s\n", reg,
                  rc, static_cast<unsigned>(val), static_cast<unsigned>(got), v);
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

        // KSCFG is U,PV: with MODEN=0 the channel is inaccessible for read AND write
        // except through KSCFG, so a dropped write measured before this point would be a
        // confound, not a result.
        r32(win + off::KSCFG) = KSCFG_MODEN | KSCFG_BPMODEN;
        // RM p.18-165: read KSCFG back before touching other USIC registers to flush the
        // control-block pipeline; the barrier keeps the volatile read from being elided.
        uint32_t const kscfg = r32(win + off::KSCFG);
        __asm volatile("" : : "r"(kscfg) : "memory");
        show("unpriv", "KSCFG", kscfg); // BPMODEN is a write-enable and reads back 0

        kos::print("[pvprobe] baseline through the seam: pattern B\n");
        seam_write("FDR", win, off::FDR, FDR_B);
        seam_write("BRG", win, off::BRG, BRG_B);
        seam_write("CCR", win, off::CCR, CCR_B);
        r32(win + off::SCTR) = SCTR_B; // U,PV: a direct store is the baseline here

        // Direct stores of pattern A. FDR/BRG/CCR must read back as B (dropped); the
        // SCTR control must read back as A (landed).
        unpriv_write("SCTR[U,PV control]", win + off::SCTR, SCTR_A);
        unpriv_write("FDR[PV]", win + off::FDR, FDR_A);
        unpriv_write("BRG[PV]", win + off::BRG, BRG_A);
        unpriv_write("CCR[PV]", win + off::CCR, CCR_A);

        // The same three registers, same thread, same window, through the seam: these
        // must now read back as A.
        kos::print("[pvprobe] pattern A through the seam (expect exact)\n");
        seam_write("FDR", win, off::FDR, FDR_A);
        seam_write("BRG", win, off::BRG, BRG_A);
        seam_write("CCR", win, off::CCR, CCR_A);

        // Out of MASK on an allowlisted register: CCR is tabled and this thread holds the
        // window, but TBIEN(13) is outside CCR's granted MODE[3:0]|RIEN|AIEN. The seam
        // must refuse the whole word instead of trimming it, so CCR still reads pattern A
        // afterwards. A partial store would leave MODE set and TBIEN clear, which reads
        // identically to a refusal without this read-back.
        uint32_t const ccr_pre = r32(win + off::CCR);
        int const off_mask = kos_periph_reg_write(win, off::CCR, CCR_MODE_SSC | CCR_TBIEN);
        uint32_t const ccr_post = r32(win + off::CCR);
        char const* mask_v = "CHANGED (value was not refused whole)";
        if (ccr_post == ccr_pre)
        {
            mask_v = "unchanged";
        }
        char s1[128];
        ksnprintf(s1, sizeof(s1),
                  "[pvprobe] mask refusal: CCR|TBIEN rc=%d (want -%d), pre=0x%x post=0x%x %s\n",
                  off_mask, KOS_EINVAL, static_cast<unsigned>(ccr_pre),
                  static_cast<unsigned>(ccr_post), mask_v);
        kos::print(s1);

        // Off the allowlist: same held window, an offset the chip does not table. SCTR is
        // U,PV, so it is writable directly and the seam tables it nowhere.
        int const off_list = kos_periph_reg_write(win, off::SCTR, SCTR_B);
        // The sibling channel, whose window this thread does NOT hold: the possession
        // gate must refuse before the chip table is consulted.
        int const unheld = kos_periph_reg_write(U0C0_BASE, off::FDR, FDR_B);
        char s2[120];
        ksnprintf(s2, sizeof(s2),
                  "[pvprobe] refusals: off-allowlist rc=%d (want -%d), unheld-window rc=%d (want -%d)\n",
                  off_list, KOS_EINVAL, unheld, KOS_EPERM);
        kos::print(s2);

        kos::print("[pvprobe] poking UNGRANTED SCU @ 0x50004648 (expect MPU FAULT)\n");
        uint32_t const leaked = r32(SCU_CGATCLR0);

        char s[112];
        ksnprintf(s, sizeof(s),
                  "[pvprobe] UNGRANTED ACCESS DID NOT FAULT (SCU=0x%x): MPU not enforcing\n",
                  static_cast<unsigned>(leaked));
        kos::print(s);
        kos_panic("[pvprobe] isolation FAILURE: ungranted read landed");
    }
}

int main(int, char**)
{
    // No register access from root: it holds no DEV region, so a store here would
    // MemManage before the probe ever ran.
    kos::print("[pvprobe] XMC4800 U0C1 PV-write probe (RM V1.3 Table 18-20)\n");

    // The probe ends on the negative control's fault, and a fault cancels the faulting
    // thread's whole TASK: spawned plain it would join root's task and take root with it,
    // leaving no survivor to keep the board up. Root holds the handle for the life of the
    // image, since it never reaches a point past the probe.
    kos_task_t victim = KOS_TASK_NONE;
    if (kos_task_create(nullptr, 0, 0, &victim) != 0)
    {
        kos_panic("[pvprobe] no task slot for the probe");
    }

    auto const p = kos::thread::create(probe, reinterpret_cast<void*>(U0C1_BASE),
                                       "pvprobe", 10, KOS_POLICY_FIFO, 0,
                                       /*privileged=*/false,
                                       /*mem=*/nullptr, /*mem_size=*/0,
                                       /*stack=*/nullptr, /*stack_size=*/0,
                                       /*mmio=*/reinterpret_cast<void*>(U0C1_BASE),
                                       U0C1_WINDOW,
                                       /*caps=*/nullptr, /*cap_count=*/0,
                                       /*authority=*/0, /*cap_dest=*/nullptr, victim);
    if (not p.valid())
    {
        // -KOS_EBUSY: a live domain already holds U0C1, and without the grant the probe
        // question is unanswerable. The errno goes out through the panic path because the
        // kernel console path drops every byte once a driver has published.
        char e[64];
        ksnprintf(e, sizeof(e), "[pvprobe] U0C1 probe spawn refused, errno %d", -p.error());
        kos_panic(e);
    }

    // Park. The sleep fallback covers a semaphore that could not be created, since an
    // unmintable handle would spin a hot loop of failing sem_wait syscalls.
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
