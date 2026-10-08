// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RXv3 core + on-chip register fields the arch backend touches (ICUD, the two CMTW units,
// MPU), from the RX72M Group User's Manual: Hardware (r01uh0804ej0120, Rev.1.20). No chip
// address belongs here: a host test includes this header.

#ifndef KICKOS_ARCH_RX_RXV3_REGS_H
#define KICKOS_ARCH_RX_RXV3_REGS_H

#include <stdint.h>

#include <kickos/arch/rx_group.h>

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

        // Initial thread PSW, IPL=0. Both must carry U with I: switch.S relies on I=1
        // implying U=1 everywhere in arch/rx.
        constexpr uint32_t PSW_THREAD_KERNEL = PSW_I | PSW_U;
        constexpr uint32_t PSW_THREAD_USER = PSW_I | PSW_U | PSW_PM;

        // MVTIPL to IPL_LOCK masks every source at or below it, so device and timer lines
        // must be programmed BELOW it.
        constexpr uint32_t IPL_LOCK = 12;
        constexpr uint32_t IPL_DEVICE = 4;  // default device/timer priority (< lock)

        // --- ICUD interrupt controller (HW UM sec.15) ---
        // SWINTR.SWINT pends the software interrupt (SWINT, vector 27), SWINT2R.SWINT2 the
        // second (vector 26) (UM sec.15.2.5 p.484). They are the only lines software can raise
        // (edge sources accept only a 0 write to IRn.IR).
        constexpr uint8_t SWINTR_SWINT = 1u << 0;
        constexpr int SWINT_VECTOR = 27;
        // SWINT is the deferred-switch line (the PendSV analog): give it the lowest active
        // priority so it is accepted only after every other ISR drains.
        constexpr uint32_t IPL_PENDSW = 1; // lowest active level (< IPL_LOCK)
        // SWINT2 is the arch_irq_inject doorbell.
        constexpr uint8_t SWINT2R_SWINT2 = 1u << 0;
        constexpr int SWINT2_VECTOR = 26;

        // --- Compare Match Timer W (HW UM sec.32, p.1608 ff) ---
        // Two 32-bit up-counters. Unit 0 = one-shot next-event timer; unit 1 =
        // free-running monotonic clock.
        constexpr uintptr_t CMTW_CMWSTR = 0x00; // 16-bit: b0 STR start/stop
        constexpr uintptr_t CMTW_CMWCR = 0x04;  // 16-bit: control (clock, clear src)
        constexpr uintptr_t CMTW_CMWIOR = 0x08; // 16-bit: I/O + compare-match enable
        constexpr uintptr_t CMTW_CMWCNT = 0x10; // 32-bit: counter
        constexpr uintptr_t CMTW_CMWCOR = 0x14; // 32-bit: compare-match constant
        constexpr uint16_t CMWSTR_STR = 1u << 0;
        // CMWIOR.CMWE (b15) GATES the CMWCOR compare-match operation: with CMWE=0
        // (reset) the counter never matches CMWCOR, so it neither clears (CCLR=000) nor
        // raises CMWI. It must be set for the one-shot timer (UM sec.32.2.3). CMTW1
        // (free-running clock) does no compare, so it omits it.
        constexpr uint16_t CMWIOR_CMWE = 1u << 15;
        // CMWCR fields (UM sec.32.2.2): CKS[1:0]=b1:0 clock select (00 => PCLK/8);
        // CMWIE=b3 compare-match interrupt enable; CMS=b9 counter size (0 => 32-bit);
        // CCLR[2:0]=b15:13 clear source (000 => cleared by CMWCOR compare match,
        // 001 => clearing disabled / free-running).
        constexpr uint16_t CMWCR_CKS_PCLK8 = 0x0000;      // CKS=00: PCLK/8
        constexpr uint16_t CMWCR_CMWIE = 1u << 3;         // compare-match interrupt
        constexpr uint16_t CMWCR_CCLR_ON_MATCH = 0x0000;  // CCLR=000: clear on CMWCOR
        constexpr uint16_t CMWCR_CCLR_FREERUN = 0x2000;   // CCLR=001: no clear (free-run)

        // --- Memory-Protection Unit (HW UM sec.17) ---
        // Eight access-control regions (n=0..7) + one background region. Page granularity
        // is 16 bytes: the page number is address[31:4], carried in bits[31:4] of the start
        // and end page registers (UM sec.17.1.2). The end page is INCLUSIVE (UM sec.17.2.2).
        constexpr uintptr_t MPU_REGION_STRIDE = 8;
        constexpr size_t MPU_REGION_COUNT = 8;
        // REPAGEn low bits (UM sec.17.2.2): V = region-valid, UAC[2:0] user-mode
        // access = b3 Read / b2 Write / b1 Execute (1 = permitted). Note the bit
        // order: read is the HIGH bit, execute the low, NOT r/w/x LSB-first.
        constexpr uint32_t MPU_REPAGE_V = 1u << 0;
        constexpr uint32_t MPU_UAC_R = 1u << 3;
        constexpr uint32_t MPU_UAC_W = 1u << 2;
        constexpr uint32_t MPU_UAC_X = 1u << 1;
        constexpr uint32_t MPU_PAGE_MASK = 0xFFFFFFF0u; // address -> page bits[31:4]
        // MPEN b0: global enable; address checking begins on the RTE/RTFI that next shifts
        // to user mode (UM sec.17.2.3). MPBAC UBAC[2:0]: background user-mode access, same
        // b3/b2/b1 layout as UAC, 0 => user has NO access outside an explicit region.
        // MPOPI b0: writing 1 clears the V bit of every region (UM sec.17.2.10). MPU
        // registers are supervisor-only and are NOT gated by PRCR (UM Table 13.1 omits them).
        constexpr uint32_t MPU_MPEN_MPEN = 1u << 0;
        constexpr uint16_t MPU_MPOPI_INV = 1u << 0;
        // Error status for the access-exception reporter (UM sec.17.2.5-7): MPECLR.CLR
        // clears the latched status; MPESTS IMPER = instruction-fetch violation, DMPER =
        // operand-access violation, DRW = 1 write / 0 read (valid only when DMPER=1);
        // MPDEA holds the operand-access faulting address (the fetch address is the
        // stacked PC).
        constexpr uint32_t MPU_MPECLR_CLR = 1u << 0;
        constexpr uint32_t MPU_MPESTS_IMPER = 1u << 0;
        constexpr uint32_t MPU_MPESTS_DMPER = 1u << 1;
        constexpr uint32_t MPU_MPESTS_DRW = 1u << 2;

        // --- Syscall trap vector ---
        // switch.S hard-codes INT #1. Slots 0..15 of the INTB table carry no peripheral
        // source (IRn exists only for 16..255), so a low number cannot collide with a
        // device line. Vector 0 is BRK.
        constexpr int SYSCALL_VECTOR = 1;
    }
}

#endif
