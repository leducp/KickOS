// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M SCI register offsets + fields. From the RX72M Group User's Manual: Hardware
// (r01uh0804ej0120, Rev.1.20) sec.42; hand-rolled, clean-room. Bases: mmap.h.
//
// Offsets are INSTANCE-RELATIVE; the channel stride is 0x20 for SCI0..SCI6.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_REGS_SCI_H
#define KICKOS_ARCH_RX_CHIP_RX72M_REGS_SCI_H

#include <stdint.h>

#include <kickos/chip_mmap.h>

namespace kickos::rx::reg::sci
{
    // Register offsets from a channel base (UM sec.42.2).
    constexpr uintptr_t SMR_OFFSET = 0x00;  // serial mode (sec.42.2.9 p.2163)
    constexpr uintptr_t BRR_OFFSET = 0x01;  // bit rate (sec.42.2.13 p.2181)
    constexpr uintptr_t SCR_OFFSET = 0x02;  // serial control (sec.42.2.10 p.2167)
    constexpr uintptr_t TDR_OFFSET = 0x03;  // transmit data
    constexpr uintptr_t SSR_OFFSET = 0x04;  // serial status (sec.42.2.11 p.2171)
    constexpr uintptr_t RDR_OFFSET = 0x05;  // receive data (sec.42.2.2 p.2157)
    constexpr uintptr_t SCMR_OFFSET = 0x06; // smart card mode (sec.42.2.12 p.2179)
    constexpr uintptr_t SEMR_OFFSET = 0x07; // serial extended mode (sec.42.2.15 p.2195)

    // SCI6, the board console channel. Absolute, for the kernel console path only.
    constexpr uintptr_t SMR = mmap::SCI6 + SMR_OFFSET;
    constexpr uintptr_t BRR = mmap::SCI6 + BRR_OFFSET;
    constexpr uintptr_t SCR = mmap::SCI6 + SCR_OFFSET;
    constexpr uintptr_t TDR = mmap::SCI6 + TDR_OFFSET;
    constexpr uintptr_t SSR = mmap::SCI6 + SSR_OFFSET;
    constexpr uintptr_t RDR = mmap::SCI6 + RDR_OFFSET;
    constexpr uintptr_t SCMR = mmap::SCI6 + SCMR_OFFSET;
    constexpr uintptr_t SEMR = mmap::SCI6 + SEMR_OFFSET;

    // SCR fields, asynchronous mode (UM sec.42.2.10 p.2167).
    constexpr uint8_t SCR_TIE = 1u << 7;  // TXI interrupt enable
    constexpr uint8_t SCR_RIE = 1u << 6;  // RXI *and* ERI interrupt enable
    constexpr uint8_t SCR_TE = 1u << 5;   // transmit enable
    constexpr uint8_t SCR_RE = 1u << 4;   // receive enable
    constexpr uint8_t SCR_TEIE = 1u << 2; // TEI interrupt enable
    constexpr uint8_t SCR_CKE_MASK = 0x3; // 00 selects the on-chip baud rate generator

    // TXI is raised only by a TDR -> TSR transfer taken with TIE ALREADY 1 (UM
    // sec.42.12.2(1) p.2308); setting TIE while TE is 1 raises nothing, so TIE must be
    // armed before the first TDR write or TX never starts. Clearing TIE also discards the
    // request the SCI retains internally (sec.42.12.1(1) p.2308), so clear it only with
    // nothing left to send. Masking at the ICU is not a substitute: a tier-1 rearm unmasks
    // on every wait.

    // SSR fields, asynchronous mode (UM sec.42.2.11 p.2171).
    constexpr uint8_t SSR_TDRE = 1u << 7; // transmit-data-empty
    constexpr uint8_t SSR_RDRF = 1u << 6; // receive-data-full; cleared by READING RDR
    constexpr uint8_t SSR_ORER = 1u << 5; // overrun
    constexpr uint8_t SSR_FER = 1u << 4;  // framing
    constexpr uint8_t SSR_PER = 1u << 3;  // parity
    constexpr uint8_t SSR_TEND = 1u << 2; // transmission end; READ-ONLY
    constexpr uint8_t SSR_ERRORS = SSR_ORER | SSR_FER | SSR_PER;
    // ORER/FER/PER clear by writing 0 to a bit that read 1 (Note 1 p.2171) while TDRE and
    // RDRF must be written as 1 (Note 2), so storing back the byte just read is wrong both
    // ways. Reception is STOPPED while any of the three is 1 (sec.42.3.9 p.2241), and
    // recovering from an overrun also requires READING RDR (Fig.42.21 step [6] p.2243).

    // SMR fields, asynchronous mode (UM sec.42.2.9 p.2163). Writable ONLY with SCR.TE and
    // SCR.RE both 0 (Note 4).
    constexpr uint8_t SMR_CKS_MASK = 0x3; // 00=PCLK, 01=PCLK/4, 10=PCLK/16, 11=PCLK/64
    constexpr uint8_t SMR_MP = 1u << 2;
    constexpr uint8_t SMR_STOP = 1u << 3; // 0 = one stop bit, 1 = two
    constexpr uint8_t SMR_PM = 1u << 4;   // 0 = even, 1 = odd; valid only with PE
    constexpr uint8_t SMR_PE = 1u << 5;   // parity is an EXTRA bit, not a stolen data bit
    constexpr uint8_t SMR_CHR = 1u << 6;  // pairs with SCMR.CHR1 for the character length
    constexpr uint8_t SMR_CM = 1u << 7;   // 0 = asynchronous

    // SCMR (UM sec.42.2.12 p.2179), reset F2h, writable only with TE and RE both 0. b6, b5
    // and b1 are reserved READ-AS-1 whose write value must be 1, so write this register as
    // a whole constant, never read-modify-write.
    //
    // Character length is the (SCMR.CHR1, SMR.CHR) PAIR: (1,0) = 8 bits, (1,1) = 7 bits,
    // CHR1 0 = 9 bits whatever CHR is.
    constexpr uint8_t SCMR_SMIF = 1u << 0; // smart card interface mode
    constexpr uint8_t SCMR_SINV = 1u << 2; // invert transmitted/received data
    constexpr uint8_t SCMR_SDIR = 1u << 3; // 0 = LSB first
    constexpr uint8_t SCMR_CHR1 = 1u << 4;
    constexpr uint8_t SCMR_UART = 0xF2; // reset value: SMIF 0, SINV 0, SDIR 0, CHR1 1

    // SEMR fields (UM sec.42.2.15 p.2195), writable only with TE and RE both 0 (Note 1).
    constexpr uint8_t SEMR_ACS0 = 1u << 0;
    constexpr uint8_t SEMR_BRME = 1u << 2;  // bit rate modulation, via MDDR at +0x12
    constexpr uint8_t SEMR_ABCSE = 1u << 3; // 6 base clocks per bit
    constexpr uint8_t SEMR_ABCS = 1u << 4;  // 8 base clock cycles per bit (else 16)
    constexpr uint8_t SEMR_NFEN = 1u << 5;
    constexpr uint8_t SEMR_BGDM = 1u << 6;    // baud generator double-speed
    constexpr uint8_t SEMR_RXDESEL = 1u << 7; // 1 = falling edge starts a frame

    // --- Asynchronous baud arithmetic (UM Table 42.11 p.2181) --------------------------
    // The printed relation is N = PCLK / (K * 2^(2n-1) * B) - 1, with K = 64, 32 or 16 as
    // BGDM and ABCS are set, K = 12 whenever ABCSE is, and n = SMR.CKS. 2^(2n-1) is a HALF
    // integer at n = 0, so K and it fold into ONE divider D = (K/2) << 2n, giving
    // B = PCLK / (D * (N + 1)) with no fractional step anywhere.
    //
    // The SCI clock is PCLKB for SCI0..SCI6 and SCI12 (UM sec.42 preamble p.2144).

    // D for a live (CKS, SEMR) pair.
    inline uint32_t baud_divider(uint8_t cks, uint8_t semr)
    {
        uint32_t base = 32u;
        if ((semr & SEMR_BGDM) != 0)
        {
            base >>= 1;
        }
        if ((semr & SEMR_ABCS) != 0)
        {
            base >>= 1;
        }
        if ((semr & SEMR_ABCSE) != 0)
        {
            base = 6u; // K = 12 at a doubled generator output (sec.42.3.4 p.2227)
        }
        return base << (2u * (cks & SMR_CKS_MASK));
    }

    struct BaudSetting
    {
        uint8_t cks;  // goes into SMR.CKS[1:0]
        uint8_t semr; // the BGDM and ABCS bits alone
        uint8_t brr;
    };

    // Half a bit of slip by the last bit of a 10-bit frame is ~5% shared between the two
    // ends of a link, so 2% is one end's share. Without this gate a 4 Mbaud request off
    // 60 MHz is answered with a perfectly legal 7.5 Mbaud divisor.
    constexpr uint32_t BAUD_TOLERANCE_PCT = 2u;

    // The best divisor for `baud` off `clk`, or false when no candidate lands inside the
    // tolerance. Both N = floor(clk / (D * baud)) AND N + 1 are scored per
    // (CKS, BGDM, ABCS) family, bounds-checked on their own, and the smallest absolute
    // error wins. SCORING THE FLOOR ALONE IS WRONG BY CONSTRUCTION: the achieved rate is
    // clk / (D * N), so the ceil candidate is strictly closer whenever the exact quotient's
    // fraction exceeds one half.
    //
    // Ties go to ABCS = 0, which is 16 base clocks per bit against 8 (sec.42.2.15 p.2197),
    // for twice the receiver's sampling margin. The candidate ORDER and the strict
    // comparison are what encode that, so neither is free to change.
    //
    // ABCSE is never selected: its D = 6 << 2n sits between the D = 8 and D = 16 rows.
    inline bool baud_select(uint32_t clk, uint32_t baud, BaudSetting* out)
    {
        if (clk == 0u or baud == 0u)
        {
            return false;
        }
        uint8_t const steps[3] = {SEMR_BGDM, 0, static_cast<uint8_t>(SEMR_BGDM | SEMR_ABCS)};
        bool found = false;
        uint32_t best_err = 0;
        for (uint8_t cks = 0; cks < 4u; cks++)
        {
            for (unsigned i = 0; i < 3u; i++)
            {
                uint64_t const d = baud_divider(cks, steps[i]);
                uint64_t const floor_n = clk / (d * static_cast<uint64_t>(baud));
                for (unsigned k = 0; k < 2u; k++)
                {
                    uint64_t const n = floor_n + k;
                    if (n == 0u or n > 256u)
                    {
                        continue;
                    }
                    uint32_t const got = static_cast<uint32_t>(clk / (d * n));
                    uint32_t err = got - baud;
                    if (got < baud)
                    {
                        err = baud - got;
                    }
                    if (found and err >= best_err)
                    {
                        continue;
                    }
                    found = true;
                    best_err = err;
                    out->cks = cks;
                    out->semr = steps[i];
                    out->brr = static_cast<uint8_t>(n - 1u);
                }
            }
        }
        if (not found)
        {
            return false;
        }
        return static_cast<uint64_t>(best_err) * 100u
               <= static_cast<uint64_t>(baud) * BAUD_TOLERANCE_PCT;
    }

    // The rate a channel is actually running at, from its own SMR, SEMR and BRR.
    // Truncating, so the answer is the achieved rate rounded DOWN.
    inline uint32_t baud_achieved(uint32_t clk, uint8_t smr, uint8_t semr, uint8_t brr)
    {
        uint32_t const d = baud_divider(smr & SMR_CKS_MASK, semr);
        return clk / (d * (static_cast<uint32_t>(brr) + 1u));
    }
}

#endif
