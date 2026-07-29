// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 cross-channel interrupt-injection probe. An unprivileged thread holding a
// legitimate MPU grant for USIC0 channel 1 (0x4003_0200, size 0x200) reroutes that
// channel's receive interrupt onto the kernel console's service-request node and
// storms it, to test whether it can deny service to the console.
//
// Mechanism (all in-window, all U,PV writable, no privileged access needed):
//   * The kernel console TX drains on U0C0 -> USIC0 SR0 -> NVIC 84, bound as an
//     in-kernel handler (console_tx.cc console_tx_isr).
//   * INPR (channel-window offset 0x018) selects the SRx node for RIF/AIF. RINP/AINP
//     field 0 selects SR0 -- the console's node. INPR is U,PV and in-window.
//   * DX0CR internal loopback makes every TBUF0 write produce a receive event; root
//     armed CCR.RIEN|AIEN (CCR is write-PV-only, so the arming must be privileged).
//   * The attacker reroutes INPR to SR0 and writes TBUF0 in a tight loop, injecting
//     U0C1 receive events into the kernel console's ISR.
//
// The attacker is spawned BELOW root's priority, so it can never starve root by
// hogging the CPU: a wedged console would isolate the foreign-SR0 interrupt storm as
// the cause.
//
// MEASURED ON SILICON (XMC4800-Relax, 2026-07-28): the reroute lands (INPR
// 0x1100 -> 0x0) and the injection is real -- with the kernel console_tx_isr
// instrumented, the foreign events drove it at ~37,700 invocations/second. But the
// console KEPT BEATING and the board did NOT wedge. A separate run left the RIF/AIF
// flag set and the attacker idle: console_tx_isr took ZERO extra entries. So the XMC
// USIC receive service request is EDGE (one pulse per received word), not the
// sustained level the DoS premise assumes -- a held, uncleared flag does not
// re-assert SR0, and console_tx_isr always returns. The injection is a bounded CPU
// tax contingent on the attacker continuously running, NOT a denial of service.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference
// Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. Diagnostic app
// (kickos_add_diagnostic_app): build-only here, validated by the operator on silicon.
// The kernel console_tx path is exercised only when the console stays kernel-owned,
// so build this with the kernel-console service list (KICKOS_SERVICE_LIST=
// kickos_services_none); under the xmcuart handover the kernel path is deinit'd.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <regs/usic.h> // shared XMC USIC register offsets + SSC bit fields

#include <stdint.h>

#if !KICKOS_HAVE_MPU
#error "inprstorm requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

using namespace kickos::xmc::reg::usic;

namespace
{
    constexpr uint32_t U0C1_WINDOW = 0x200u;

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

    void storm(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        kos::print("[inprstorm] unpriv up (granted U0C1 window 0x200)\n");

        // The U,PV subset root already left in place; rewriting it mirrors a real
        // driver bring-up. FDR/BRG/CCR are PV-write-only and are dropped from an
        // unprivileged store, so the arming (CCR.RIEN|AIEN) had to come from root.
        r32(win + off::SCTR) = SCTR_TRM_ACTIVE | SCTR_WLE_8 | SCTR_FLE_8 | SCTR_SDIR_MSB;
        r32(win + off::TCSR) = TCSR_TDEN_TDV | TCSR_TDSSM;
        r32(win + off::PCR) = PCR_MSLSEN;
        r32(win + off::DX0CR) = DX0CR_INSW | DX0CR_DSEL_G; // internal loopback
        r32(win + off::PSCR) = PSCR_CRIF | PSCR_CAIF;      // clear stale RX flags

        // Hold so root prints several heartbeats that DO drain -- the console is
        // demonstrably alive right up to the storm.
        kos_sleep_ns(2000000000ull);

        // The attack: RINP/AINP field 0 routes the receive interrupt to SR0, the
        // console's node. INPR is U,PV and in-window, so this unprivileged store lands.
        kos::print("[inprstorm] rerouting INPR RINP/AINP -> SR0 (console node)\n");
        show("INPR before", r32(win + off::INPR));
        r32(win + off::INPR) = 0u;
        show("INPR after ", r32(win + off::INPR));
        show("CCR        ", r32(win + off::CCR));

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
    }
}

int main(int, char**)
{
    kos::print("[inprstorm] XMC4800 console DoS probe via U0C1 INPR reroute onto SR0\n");
    kos::print("[inprstorm] MARKER: root up, arming U0C1 as SSC master + loopback\n");

    // Root bring-up (privileged): kernel clock, baud (FDR/BRG PV-write-only), SSC
    // config, internal loopback, receive-interrupt arm. INPR routed to the benign SR1
    // (NVIC 85, masked, no driver) so nothing storms until the unprivileged thread
    // reroutes it. Mirrors system/driver/xmc4800/xmcssc/xmcssc.cc bring-up.
    r32(U0C1_BASE + off::KSCFG) = KSCFG_MODEN | KSCFG_BPMODEN;
    uint32_t const kscfg = r32(U0C1_BASE + off::KSCFG);
    __asm volatile("" : : "r"(kscfg) : "memory");

    r32(U0C1_BASE + off::FDR) = FDR_DM_FRACTIONAL | FDR_STEP_367;
    r32(U0C1_BASE + off::BRG) = BRG_PDIV_13 | BRG_DCTQ_15 | BRG_PCTQ_0;
    r32(U0C1_BASE + off::SCTR) = SCTR_TRM_ACTIVE | SCTR_WLE_8 | SCTR_FLE_8 | SCTR_SDIR_MSB;
    r32(U0C1_BASE + off::TCSR) = TCSR_TDEN_TDV | TCSR_TDSSM;
    r32(U0C1_BASE + off::PCR) = PCR_MSLSEN;
    r32(U0C1_BASE + off::PSCR) = PSCR_CRIF | PSCR_CAIF;
    r32(U0C1_BASE + off::DX0CR) = DX0CR_INSW | DX0CR_DSEL_G;
    r32(U0C1_BASE + off::INPR) = INPR_RINP_SR1 | INPR_AINP_SR1;
    r32(U0C1_BASE + off::CCR) = CCR_MODE_SSC | CCR_RIEN | CCR_AIEN;

    // Priority 1 (KICKOS_PRIO_MIN) is BELOW root's KICKOS_PRIO_MIN+1: the storm thread
    // can never starve root by hogging the CPU, so a wedged console would isolate the
    // foreign-SR0 interrupt storm as the cause.
    int const p = kos::thread::spawn(storm, reinterpret_cast<void*>(U0C1_BASE),
                                     "inprstorm", 1, KOS_POLICY_FIFO, 0,
                                     /*privileged=*/false,
                                     /*mem=*/nullptr, /*mem_size=*/0,
                                     /*stack=*/nullptr, /*stack_size=*/0,
                                     /*mmio=*/reinterpret_cast<void*>(U0C1_BASE),
                                     U0C1_WINDOW);
    if (p < 0)
    {
        kos::print("[inprstorm] ERROR: storm spawn failed\n");
    }

    // Heartbeat: if the storm were a DoS the log would die here. On silicon it keeps
    // printing -- the injection is a bounded CPU tax, not a denial of service.
    uint32_t beat = 0;
    while (true)
    {
        char s[64];
        ksnprintf(s, sizeof(s), "[inprstorm] heartbeat %u\n", static_cast<unsigned>(beat));
        kos::print(s);
        beat++;
        kos_sleep_ns(300000000ull);
    }
}
