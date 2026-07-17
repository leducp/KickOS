// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal RXv3 core + on-chip peripheral register definitions the arch backend
// touches: the ICUD interrupt controller (IR/IER/IPR/SWINTR) and the two CMTW
// units (one-shot timer + monotonic clock). Addresses are transcribed from the
// RX72M Group User's Manual: Hardware (r01uh0804ej0120, Rev.1.20) with section
// citations -- hand-rolled, clean-room (no vendor SDK), like the arm regs.h.

#ifndef KICKOS_ARCH_RX_RXV3_REGS_H
#define KICKOS_ARCH_RX_RXV3_REGS_H

#include <stdint.h>

namespace kickos
{
    namespace rxv3
    {
        inline volatile uint32_t& reg32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
        inline volatile uint16_t& reg16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }
        inline volatile uint8_t& reg8(uintptr_t a) { return *reinterpret_cast<volatile uint8_t*>(a); }

        // --- PSW bit positions (ISA UM sec.1.2.2.4 / HW UM sec.2.2.2.5) ---
        constexpr uint32_t PSW_I = 1u << 16;  // interrupt enable
        constexpr uint32_t PSW_U = 1u << 17;  // stack-pointer select (1 => USP)
        constexpr uint32_t PSW_PM = 1u << 20; // processor mode (1 => user)
        constexpr uint32_t PSW_IPL_SHIFT = 24;
        constexpr uint32_t PSW_IPL_MASK = 0xFu << PSW_IPL_SHIFT;

        // Initial PSW for a freshly-entered thread: IPL=0, I=1, U=1 (own stack).
        // Kernel adds nothing (PM=0 => supervisor); user ORs PSW_PM.
        constexpr uint32_t PSW_THREAD_KERNEL = PSW_I | PSW_U;             // PM=0,U=1,I=1
        constexpr uint32_t PSW_THREAD_USER = PSW_I | PSW_U | PSW_PM;      // PM=1,U=1,I=1

        // Kernel critical-section IPL: raising PSW.IPL to this level via MVTIPL
        // masks every source at or below it, leaving a higher band + NMI (IPL 15)
        // live -- the BASEPRI-band analog. Device lines are programmed BELOW this
        // level (see PRIO_DEVICE); the timer sits just under the lock too.
        constexpr uint32_t IPL_LOCK = 12;   // crit-section mask level
        constexpr uint32_t IPL_DEVICE = 4;  // default device/timer priority (< lock)

        // --- ICUD interrupt controller (HW UM sec.15) ---
        // IRn per-source request flag (n = 16..255), one byte each (UM sec.15.2.1 p.479):
        //   ICU.IR016 @ 0008 7010h .. ICU.IR255 @ 0008 70FFh  => IR[n] = 0x87000 + n
        constexpr uintptr_t ICU_IR_BASE = 0x00087000;
        // IERm enable registers, one bit per source (UM sec.15.2.2 p.481): line n is
        //   IER[n>>3] bit (n & 7), IER base 0008 7200h.
        constexpr uintptr_t ICU_IER_BASE = 0x00087200;
        // IPRr 4-bit per-source priority (UM sec.15.2.4 p.482). NOTE: the IPR index is
        // NOT the vector number in general -- the ICUD shares IPR entries per a
        // source table (e.g. CMWI0 vector 30 => IPR006). arch_irq_unmask maps
        // vector -> IPR index via vector_to_ipr (arch_rxv3.cc); the lines the chip
        // programs directly (the CMTW timer + SWINT) use the documented index.
        constexpr uintptr_t ICU_IPR_BASE = 0x00087300;
        // Software interrupt generation (UM sec.15.2.5 p.484): writing 1 to SWINTR.SWINT
        // pends the software interrupt (SWINT, vector 27) -- the only line software
        // can raise (edge sources accept only a 0 write to IRn.IR).
        constexpr uintptr_t ICU_SWINTR = 0x000872E0;
        constexpr uint8_t SWINTR_SWINT = 1u << 0;
        constexpr int SWINT_VECTOR = 27; // ICU.SWINTR -> IR027 (UM sec.15.2.5)
        // SWINT is the deferred-switch line (the PendSV analog, spike sec.2): give it
        // the lowest active priority so it is accepted only after every other ISR
        // drains, and enable it in kickos_rxv3_init. SWINT (27) and SWINT2 (26)
        // SHARE one IPR register, ICU.IPR[3] (RX72x UM interrupt table; confirmed
        // in the Renesas BSP: "SWINT2,SWINT share IPR level"). SWINT's IER bit is
        // IER[3].IEN3, SWINT2's is IER[3].IEN2 (both in the 0x87203 byte).
        // FIRST-SILICON CONFIRM: this is the one switch constant not establishable
        // by construction -- an IPR left at 0 means "never accepted", so a wrong
        // address/index makes the switch silently dead on the first arch_switch.
        // Cross-checked against the Renesas RX72N BSP iodefine (same RX700 ICU as
        // RX72M): SWINTR@0x872E0, and SWINT/SWINT2 share ICU.IPR[3] @ 0x87303.
        constexpr uintptr_t ICU_IPR_SWINT = 0x00087303; // shared SWINT/SWINT2 IPR
        constexpr uint32_t IPL_PENDSW = 1;              // lowest active level (< IPL_LOCK)
        // Second software interrupt: NOT the switch line, so it is free for the
        // test-injection scaffolding (arch_irq_inject) -- injecting it runs the
        // device default handler, exercising the IRQ path without disturbing the
        // switch mechanism on SWINT.
        constexpr uintptr_t ICU_SWINT2R = 0x000872E1;
        constexpr uint8_t SWINT2R_SWINT2 = 1u << 0;
        constexpr int SWINT2_VECTOR = 26; // ICU.SWINT2R -> IR026

        // --- Compare Match Timer W (HW UM sec.32, p.1608 ff) ---
        // Two 32-bit up-counters. Unit 0 = one-shot next-event timer (SysTick
        // analog); unit 1 = free-running monotonic clock (DWT analog).
        constexpr uintptr_t CMTW0_BASE = 0x00094200;
        constexpr uintptr_t CMTW1_BASE = 0x00094280;
        constexpr uintptr_t CMTW_CMWSTR = 0x00; // 16-bit: b0 STR start/stop
        constexpr uintptr_t CMTW_CMWCR = 0x04;  // 16-bit: control (clock, clear src)
        constexpr uintptr_t CMTW_CMWIOR = 0x08; // 16-bit: I/O + compare-match enable
        constexpr uintptr_t CMTW_CMWCNT = 0x10; // 32-bit: counter
        constexpr uintptr_t CMTW_CMWCOR = 0x14; // 32-bit: compare-match constant
        constexpr uint16_t CMWSTR_STR = 1u << 0;
        // CMWIOR.CMWE (b15): the CMWCOR compare-match operation is GATED by this bit
        // -- with CMWE=0 (reset) the counter never matches CMWCOR, so it neither
        // clears (CCLR=000) nor raises CMWI. It must be set for the one-shot timer
        // (UM sec.32.2.3). CMTW1 (free-running clock) does no compare, so it omits it.
        constexpr uint16_t CMWIOR_CMWE = 1u << 15;
        // CMWCR fields (UM sec.32.2.2): CKS[1:0]=b1:0 clock select (00 => PCLK/8);
        // CMWIE=b3 compare-match interrupt enable; CMS=b9 counter size (0 => 32-bit);
        // CCLR[2:0]=b15:13 clear source (000 => cleared by CMWCOR compare match,
        // 001 => clearing disabled / free-running).
        constexpr uint16_t CMWCR_CKS_PCLK8 = 0x0000;      // CKS=00: PCLK/8
        constexpr uint16_t CMWCR_CMWIE = 1u << 3;         // compare-match interrupt
        constexpr uint16_t CMWCR_CCLR_ON_MATCH = 0x0000;  // CCLR=000: clear on CMWCOR
        constexpr uint16_t CMWCR_CCLR_FREERUN = 0x2000;   // CCLR=001: no clear (free-run)
        constexpr int CMWI0_VECTOR = 30; // CMTW0 compare match (UM interrupt table)
        constexpr int CMWI1_VECTOR = 31; // CMTW1 compare match

        // --- Memory-Protection Unit (HW UM sec.17, base 0008 6400h) ---
        // Eight access-control regions (n=0..7) + one background region. Page
        // granularity is 16 bytes: the page number is address[31:4], carried in
        // bits[31:4] of the start/end page registers (UM sec.17.1.2).
        //   RSPAGEn @ 0x86400 + n*8: RSPN[27:0] = start page = start_addr>>4,
        //     so the register value is start_addr with the low 4 bits masked off.
        //   REPAGEn @ 0x86404 + n*8: REPN[27:0] = END page (INCLUSIVE -- the end
        //     page is part of the region, UM sec.17.2.2), plus UAC[2:0] and V.
        constexpr uintptr_t MPU_RSPAGE_BASE = 0x00086400; // + region*8
        constexpr uintptr_t MPU_REPAGE_BASE = 0x00086404; // + region*8
        constexpr uintptr_t MPU_REGION_STRIDE = 8;
        constexpr size_t MPU_REGION_COUNT = 8;
        // REPAGEn low bits (UM sec.17.2.2): V = region-valid, UAC[2:0] user-mode
        // access = b3 Read / b2 Write / b1 Execute (1 = permitted). Note the bit
        // order: read is the HIGH bit, execute the low -- NOT r/w/x LSB-first.
        constexpr uint32_t MPU_REPAGE_V = 1u << 0;
        constexpr uint32_t MPU_UAC_R = 1u << 3;
        constexpr uint32_t MPU_UAC_W = 1u << 2;
        constexpr uint32_t MPU_UAC_X = 1u << 1;
        constexpr uint32_t MPU_PAGE_MASK = 0xFFFFFFF0u; // address -> page bits[31:4]
        // MPEN @ 0x86500 b0: global enable. Address checking begins on the RTE/RTFI
        // that next shifts to user mode (UM sec.17.2.3). MPBAC @ 0x86504: background
        // (whole 4 GB) user-mode access in UBAC[2:0], same b3/b2/b1 layout as UAC;
        // 0 => user has NO access outside an explicit region. MPOPI @ 0x86526 b0:
        // writing 1 clears the V bit of every region (UM sec.17.2.10). MPU registers
        // are supervisor-only and are NOT gated by PRCR (UM Table 13.1 omits them).
        constexpr uintptr_t MPU_MPEN = 0x00086500;
        constexpr uintptr_t MPU_MPBAC = 0x00086504;
        constexpr uintptr_t MPU_MPOPI = 0x00086526; // 16-bit
        constexpr uint32_t MPU_MPEN_MPEN = 1u << 0;
        constexpr uint16_t MPU_MPOPI_INV = 1u << 0;
        // Error-status decode for the access-exception reporter (UM sec.17.2.5-7).
        // MPECLR @ 0x86508 b0 CLR: write 1 clears the latched status. MPESTS @
        // 0x8650C: IMPER b0 = instruction-fetch violation, DMPER b1 = operand-access
        // violation, DRW b2 = 1 write / 0 read (valid only when DMPER=1). MPDEA @
        // 0x86514: the operand-access faulting address (the fetch address is the
        // stacked PC).
        constexpr uintptr_t MPU_MPECLR = 0x00086508;
        constexpr uintptr_t MPU_MPESTS = 0x0008650C;
        constexpr uintptr_t MPU_MPDEA = 0x00086514;
        constexpr uint32_t MPU_MPECLR_CLR = 1u << 0;
        constexpr uint32_t MPU_MPESTS_IMPER = 1u << 0;
        constexpr uint32_t MPU_MPESTS_DMPER = 1u << 1;
        constexpr uint32_t MPU_MPESTS_DRW = 1u << 2;

        // --- Syscall trap vector ---
        // arch_syscall issues INT #<this>; the handler is installed at INTB[<this>].
        // Slots 0..15 of the INTB table carry no peripheral source (IRn exists only
        // for 16..255), so a low number cannot collide with a device line. Vector 0
        // is BRK; use 1.
        constexpr int SYSCALL_VECTOR = 1;
    }
}

#endif
