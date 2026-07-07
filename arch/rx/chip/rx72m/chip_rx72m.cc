// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M chip backend. Register addresses/fields are from the RX72M Group User's
// Manual: Hardware (r01uh0804ej0120, Rev.1.20); hand-rolled (no vendor SDK),
// consistent with the arch layer's clean-room regs.h.
//
// Board: RX72M CPU Card with RDC-IC (RTK0EMXDE0C00000BJ), R5F572MNDDBD, 24 MHz
// main crystal (board UM r12uz0098ej0110 Table 1-1). Console = SCI6 on PB1/TXD6
// + PB0/RXD6 (board Table 5-4, CN6/CN7 "Renesas Motor Workbench" serial). Diag
// LED = LED6 on P80, active-low (board Table 5-9).
//
// Clock target: ICLK 240 MHz from the 24 MHz crystal via PLL (the part's max --
// UM sec.9 / datasheet fPLL 120-240, ICLK max 240). PLL VCO = 24 MHz /1 x10 = 240;
// ICLK = /1. Above 120 MHz the code flash needs one wait state, so MEMWAIT is set
// to 1 (and read back) before the PLL runs, per UM sec.9.8 case (1). Peripheral clocks
// stay inside their ceilings: PCLKA = 120 (/2, max 120), PCLKB/C/D = FCLK = BCLK =
// 60 (/4). PCLKB = 60 MHz is the SCI + CMTW clock -- unchanged from the 120 MHz
// bring-up, so the console baud and timer tick math are identical.

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_rxv3_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // CMTW input clock (PCLKB / 8) the arch clock+timer convert against. Set to
    // the achieved value once the PLL is confirmed locked (arch_init); left at
    // the LOCO reset nominal if the bring-up degrades so timing stays plausible.
    uint32_t kickos_rx_timer_hz = 30000u; // ~LOCO/8 until PLL locks

    // ICLK core clock in Hz (CMSIS-style). LOCO reset nominal until the PLL
    // bring-up in arch_init raises it to the achieved 240 MHz.
    uint32_t SystemCoreClock = 240000u; // LOCO reset nominal (raised to 240 MHz on PLL lock)
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
    inline volatile uint16_t& r16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }
    inline volatile uint8_t& r8(uintptr_t a) { return *reinterpret_cast<volatile uint8_t*>(a); }

    // Bounded-poll ceilings: a clock/console misconfiguration must degrade (fall
    // through), never hang the boot. Sized generously vs. the LOCO-clocked worst
    // case (osc/PLL settling counts run off the ~240 kHz LOCO).
    constexpr uint32_t CLOCK_POLL_LIMIT = 2000000u;
    constexpr uint32_t CONSOLE_POLL_LIMIT = 1000000u;

    // --- SYSTEM / low-power registers (UM sec.9, sec.11, sec.13) ---
    constexpr uintptr_t SYSTEM_PRCR = 0x000803FE;   // 16-bit protect register (sec.13)
    constexpr uint16_t PRCR_UNLOCK = 0xA50B;        // key 0xA5 + PRC0|PRC1|PRC3
    constexpr uint16_t PRCR_LOCK = 0xA500;          // key 0xA5, all protect bits 0
    constexpr uintptr_t SYSTEM_MEMWAIT = 0x0008101C; // 8-bit code-flash wait control (sec.9.2)
    constexpr uint8_t MEMWAIT_ONE_WAIT = 0x01;       // MEMWAIT=1: one wait (required >120 MHz)
    constexpr uintptr_t SYSTEM_MSTPCRA = 0x00080010; // 32-bit module stop A
    constexpr uintptr_t SYSTEM_MSTPCRB = 0x00080014; // 32-bit module stop B
    constexpr uint32_t MSTPA_CMTW1 = 1u << 0;       // UM sec.11 MSTPCRA b0 = CMTW unit 1
    constexpr uint32_t MSTPA_CMTW0 = 1u << 1;       // MSTPCRA b1 = CMTW unit 0
    constexpr uint32_t MSTPB_SCI6 = 1u << 25;       // MSTPCRB b25 = SCI6 (sec.11, MSTPB25)

    // --- ICU (for the CMTW0 timer line; UM sec.15) ---
    constexpr uintptr_t ICU_IER03 = 0x00087203;     // IER register holding vector 30..
    constexpr uint8_t IER03_CMWI0 = 1u << 6;        // IER03.IEN6 = vector 30 (CMWI0)
    constexpr uintptr_t ICU_IPR006 = 0x00087306;    // IPR for CMWI0 (UM interrupt table)

    // --- Clock generation circuit (UM sec.9), all PRC0-protected ---
    constexpr uintptr_t SYSTEM_SCKCR = 0x00080020;   // 32-bit system clock control (sec.9.2.1)
    constexpr uintptr_t SYSTEM_SCKCR3 = 0x00080026;  // 16-bit clock source select (sec.9.2.4)
    constexpr uintptr_t SYSTEM_PLLCR = 0x00080028;   // 16-bit PLL control (sec.9.2.5)
    constexpr uintptr_t SYSTEM_PLLCR2 = 0x0008002A;  // 8-bit PLL stop control (sec.9.2.6)
    constexpr uintptr_t SYSTEM_MOSCCR = 0x00080032;  // 8-bit main osc control (sec.9.2.8)
    constexpr uintptr_t SYSTEM_OSCOVFSR = 0x0008003C; // 8-bit stabilization flags (sec.9.2.14)
    constexpr uintptr_t SYSTEM_MOSCWTCR = 0x000800A2; // 8-bit main osc wait control (sec.9.2.17)
    constexpr uintptr_t SYSTEM_MOFCR = 0x0008C293;   // 8-bit main osc forced osc/drive (sec.9.2.19)

    constexpr uint8_t OSCOVFSR_MOOVF = 1u << 0;   // main clock oscillation stabilized
    constexpr uint8_t OSCOVFSR_PLOVF = 1u << 2;   // PLL clock oscillation stabilized

    // MODRV2[1:0]=00 selects the 20.1..24 MHz crystal drive range (sec.9.2.19); the
    // board crystal is 24 MHz. MOFXIN=0 (no forced oscillation).
    constexpr uint8_t MOFCR_XTAL_24MHZ = 0x00;
    // MSTS wait count off the LOCO: MSTS > (tMAINOSC * fLOCO_max + 16)/32 (sec.9.2.17).
    // 0x53 covers ~10 ms of crystal settling (>> the datasheet tMAINOSC), read via
    // OSCOVFSR.MOOVF afterward -- conservative since a larger count only waits longer.
    constexpr uint8_t MOSCWTCR_MSTS = 0x53;
    // PLLCR: PLIDIV[1:0]=00 (/1 -> 24 MHz input, in range; the Renesas cpurx72m
    // reference uses the same /1), PLLSRCSEL=0 (main osc), STC[5:0]=010011b (x10.0)
    // -> 240 MHz output (fPLL max). (sec.9.2.5)
    constexpr uint16_t PLLCR_PLL_240MHZ = 0x1300;
    constexpr uint8_t PLLCR2_PLL_RUN = 0x00;      // PLLEN=0 => PLL operates
    constexpr uint8_t MOSCCR_MAIN_RUN = 0x00;     // MOSTP=0 => main osc operates
    // SCKCR: FCK/4(60) ICK/1(240) BCK/4(60) PCKA/2(120) PCKB/4(60) PCKC/4(60)
    // PCKD/4(60). All within the sec.9 ceilings (ICLK<=240, PCLKA<=120, PCLKB/FCLK<=60)
    // and the N:1/1:N ratio rules. Division field: 0000=/1, 0001=/2, 0010=/4. (sec.9.2.1)
    constexpr uint32_t SCKCR_240MHZ = 0x20021222u;
    constexpr uint16_t SCKCR3_CKSEL_PLL = 0x0400; // CKSEL[2:0]=100 => PLL (sec.9.2.4)

    // Achieved-by-construction frequencies once the PLL is the system clock source.
    constexpr uint32_t ICLK_HZ = 240000000u;      // ICLK
    constexpr uint32_t PCLKB_DIV8_HZ = 7500000u;  // PCLKB(60 MHz)/8 = CMTW input

    // --- MPC (UM sec.23): pin-function mux for the SCI6 console pins ---
    constexpr uintptr_t MPC_PWPR = 0x0008C11F;    // write-protect (sec.23.2.1)
    constexpr uint8_t PWPR_PFSWE = 1u << 6;       // PFS register write enable
    constexpr uint8_t PWPR_B0WI = 1u << 7;        // PFSWE write disable
    constexpr uintptr_t MPC_PB0PFS = 0x0008C198;  // PB0 pin function (sec.23.2)
    constexpr uintptr_t MPC_PB1PFS = 0x0008C199;  // PB1 pin function
    constexpr uint8_t PFS_PSEL_SCI6 = 0x0B;       // PSEL=001011b: PB0->RXD6, PB1->TXD6 (sec.23)

    // --- I/O ports (UM sec.22) ---
    constexpr uintptr_t PORTB_PMR = 0x0008C06B;   // PORTB mode (peripheral vs GPIO)
    constexpr uint8_t PB0 = 1u << 0;              // RXD6
    constexpr uint8_t PB1 = 1u << 1;              // TXD6
    // Diag LED = LED6 on P80, active-low (board Table 5-9). PORT8 GPIO registers.
    constexpr uintptr_t PORT8_PDR = 0x0008C008;   // direction
    constexpr uintptr_t PORT8_PODR = 0x0008C028;  // output data
    constexpr uintptr_t PORT8_PMR = 0x0008C068;   // mode
    constexpr uint8_t LED6 = 1u << 0;             // P80

    // --- SCI6 (UM sec.42), board console UART ---
    constexpr uintptr_t SCI6 = 0x0008A0C0;
    constexpr uintptr_t SCI6_SMR = SCI6 + 0x00;   // serial mode
    constexpr uintptr_t SCI6_BRR = SCI6 + 0x01;   // bit rate
    constexpr uintptr_t SCI6_SCR = SCI6 + 0x02;   // serial control
    constexpr uintptr_t SCI6_TDR = SCI6 + 0x03;   // transmit data
    constexpr uintptr_t SCI6_SSR = SCI6 + 0x04;   // serial status
    constexpr uintptr_t SCI6_SEMR = SCI6 + 0x07;  // serial extended mode
    constexpr uint8_t SCR_TE = 1u << 5;           // transmit enable
    constexpr uint8_t SSR_TDRE = 1u << 7;         // transmit-data-empty
    constexpr uint8_t SEMR_ABCS = 1u << 4;        // 8 base-clock cycles per bit
    constexpr uint8_t SEMR_BGDM = 1u << 6;        // baud generator double-speed
    // Async 8N1 at 115200 from PCLKB=60 MHz. With BGDM=1, ABCS=1, SMR.CKS=00 (n=0):
    //   N = PCLKB / (16 * 2^(2n-1) * B) - 1 = 60e6/(8*115200) - 1 = 64.1 -> 64
    // Actual baud 60e6/(8*65) = 115385 (+0.16% error). (UM sec.42 async baud table.)
    constexpr uint8_t SCI6_BRR_115200 = 64;
    constexpr uint8_t SCI6_SEMR_115200 = SEMR_BGDM | SEMR_ABCS; // 0x50

    void unlock_registers(bool on)
    {
        if (on)
        {
            r16(SYSTEM_PRCR) = PRCR_UNLOCK;
        }
        else
        {
            r16(SYSTEM_PRCR) = PRCR_LOCK;
        }
    }

    bool poll_flag(uintptr_t reg, uint8_t mask, uint32_t limit)
    {
        for (uint32_t i = 0; i < limit; i++)
        {
            if ((r8(reg) & mask) != 0)
            {
                return true;
            }
        }
        return false;
    }

    // Bring ICLK to 240 MHz via the PLL (UM sec.9.6 procedure, case 1: LOCO -> PLL,
    // main clock as the PLL source). Returns false (leaving the chip on the LOCO
    // reset clock) if the oscillator or PLL never reports stable -- a degraded but
    // non-hanging boot. Must run inside the PRCR unlock (clock regs are PRC0).
    bool clock_to_pll_240mhz()
    {
        r8(SYSTEM_MOFCR) = MOFCR_XTAL_24MHZ;    // 1. crystal drive range
        r8(SYSTEM_MOSCWTCR) = MOSCWTCR_MSTS;    // 2. oscillation settling count
        r8(SYSTEM_MOSCCR) = MOSCCR_MAIN_RUN;    // 3. start the main clock oscillator
        uint8_t mosccr_rb = r8(SYSTEM_MOSCCR);  // read back before dependent writes (sec.9.2.8)
        (void)mosccr_rb;
        if (!poll_flag(SYSTEM_OSCOVFSR, OSCOVFSR_MOOVF, CLOCK_POLL_LIMIT))
        {
            return false;
        }
        // ICLK will exceed 120 MHz, so the code flash needs one wait state; set it
        // BEFORE running the PLL, per the sec.9.8 case (1) step order (step 4, ahead of
        // PLLCR). Read back so it is in effect before ICLK can rise. MEMWAIT=1 is
        // legal at any ICLK (Table 9.3), so it is harmless if the PLL never locks.
        r8(SYSTEM_MEMWAIT) = MEMWAIT_ONE_WAIT;  // 4. one wait state (>120 MHz)
        uint8_t memwait_rb = r8(SYSTEM_MEMWAIT);
        (void)memwait_rb;
        r16(SYSTEM_PLLCR) = PLLCR_PLL_240MHZ;   // 5. multiplier + input divider
        r8(SYSTEM_PLLCR2) = PLLCR2_PLL_RUN;     // 6. run the PLL
        if (!poll_flag(SYSTEM_OSCOVFSR, OSCOVFSR_PLOVF, CLOCK_POLL_LIMIT))
        {
            return false;
        }
        // Set the dividers BEFORE the source switch: while still on the LOCO the
        // divisors apply to ~240 kHz, so no peripheral overshoots when the PLL
        // becomes the source on the next write.
        r32(SYSTEM_SCKCR) = SCKCR_240MHZ;
        r16(SYSTEM_SCKCR3) = SCKCR3_CKSEL_PLL;  // 8. ICLK <- PLL
        return true;
    }

    void sci6_console_init()
    {
        // Pin mux: route PB1->TXD6, PB0->RXD6 (UM sec.23). Unlock the MPC (clear
        // B0WI, then set PFSWE), write PSEL, relock, then hand the pins to the
        // peripheral via PORTB.PMR. Without the PMR step the pins stay GPIO.
        r8(MPC_PWPR) = 0x00;              // B0WI=0
        r8(MPC_PWPR) = PWPR_PFSWE;        // PFSWE=1
        r8(MPC_PB1PFS) = PFS_PSEL_SCI6;   // TXD6
        r8(MPC_PB0PFS) = PFS_PSEL_SCI6;   // RXD6
        r8(MPC_PWPR) = PWPR_B0WI;         // relock (PFSWE=0, B0WI=1)
        r8(PORTB_PMR) |= PB1 | PB0;       // PB1,PB0 -> peripheral function

        r8(SCI6_SCR) = 0;                 // TE/RE off while configuring
        r8(SCI6_SMR) = 0;                 // async, 8-bit, no parity, 1 stop, CKS=00
        r8(SCI6_SEMR) = SCI6_SEMR_115200; // BGDM+ABCS
        r8(SCI6_BRR) = SCI6_BRR_115200;
        for (volatile uint32_t d = 0; d < 10000u; d++) // >= 1-bit settle before TE (sec.42 init)
        {
        }
        r8(SCI6_SCR) = SCR_TE;            // enable transmitter
    }
}

extern "C"
{

void arch_init(void)
{
    unlock_registers(true);
    bool on_pll = clock_to_pll_240mhz();
    // Release the module stops for the timer + console (UM sec.11 MSTPCR).
    r32(SYSTEM_MSTPCRA) &= ~(MSTPA_CMTW0 | MSTPA_CMTW1);
    r32(SYSTEM_MSTPCRB) &= ~MSTPB_SCI6;
    unlock_registers(false);

    if (on_pll)
    {
        SystemCoreClock = ICLK_HZ;
        kickos_rx_timer_hz = PCLKB_DIV8_HZ;
    }

    sci6_console_init();

    // Timer line (CMTW0 compare match, vector 30): priority below the kernel lock
    // level, then enable at the ICU. (The CMTW's own CMWIE is set per-arm.)
    r8(ICU_IPR006) = 4; // IPL_DEVICE (< IPL_LOCK)
    r8(ICU_IER03) |= IER03_CMWI0;

    kickos_rxv3_init(); // start CMTW1 free-run + reset arch software state
}

void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((r8(SCI6_SSR) & SSR_TDRE) == 0)
        {
            if (++spin >= CONSOLE_POLL_LIMIT)
            {
                return; // TDRE never cleared (SCI dead/misconfigured): drop, don't hang
            }
        }
        r8(SCI6_TDR) = static_cast<uint8_t>(buf[i]);
    }
}

void arch_diag_led_init(void)
{
    r8(PORT8_PMR) &= ~LED6;   // GPIO (not peripheral)
    r8(PORT8_PODR) |= LED6;   // drive high => LED off (active-low, board Table 5-9)
    r8(PORT8_PDR) |= LED6;    // output
}

void arch_diag_led_set(int on)
{
    if (on != 0)
    {
        r8(PORT8_PODR) &= ~LED6; // low => LED on
    }
    else
    {
        r8(PORT8_PODR) |= LED6;  // high => LED off
    }
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("mvtipl #15" ::: "memory"); // mask all maskable interrupts
    while (true)
    {
        __asm volatile("wait");
    }
}

// C runtime init, reached from _start (startup.S). Never returns.
void rx_reset_handler(void)
{
    uint32_t* src = &_sidata;
    uint32_t* dst = &_sdata;
    while (dst < &_edata)
    {
        *dst++ = *src++;
    }
    for (uint32_t* b = &_sbss; b < &_ebss; b++)
    {
        *b = 0;
    }
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
