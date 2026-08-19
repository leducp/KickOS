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
// Clock target: ICLK 240 MHz from the 24 MHz crystal via PLL (the part's max; UM sec.9,
// datasheet fPLL 120-240, ICLK max 240). PLL VCO = 24 MHz /1 x10 = 240; ICLK = /1. Above
// 120 MHz the code flash needs one wait state, so MEMWAIT is set to 1 (and read back)
// before the PLL runs, per UM sec.9.8 case (1). Peripheral clocks stay inside their
// ceilings: PCLKA = 120 (/2, max 120), PCLKB/C/D = FCLK = BCLK = 60 (/4). PCLKB = 60 MHz
// is the SCI + CMTW clock.

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h> // KOS_E* taxonomy (arch_pinmux_set)

#include <stdint.h>

// Bases in mmap.h, IRQ vectors in irq.h, per-peripheral offsets/fields in regs/.
#include <kickos/chip_mmap.h>
#include "irq.h"
#include "regs/cgc.h"
#include "regs/flash.h"
#include "regs/icu.h"
#include "regs/mpc.h"
#include "regs/port.h"
#include "regs/sci.h"

namespace mmap = kickos::rx::mmap;
namespace irq = kickos::rx::irq;
namespace cgc = kickos::rx::reg::cgc;
namespace flash = kickos::rx::reg::flash;
namespace icu = kickos::rx::reg::icu;
namespace mpc = kickos::rx::reg::mpc;
namespace port = kickos::rx::reg::port;
namespace sci = kickos::rx::reg::sci;

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_rxv3_init(void);

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

    // The kernel console's rate. A userspace driver taking the channel over asks for its
    // own through kos_uart_config and is told what it actually got.
    constexpr uint32_t CONSOLE_BAUD = 115200u;

    // The window arch_console_reclaim reports. 16 bytes is the RX MPU minimum region
    // (arch_mpu_min_region), and it stops short of SPTR at +0x1C, whose SPB2IO/SPB2DT pair
    // drives TXD6 low while SCR.TE is 0 (UM Table 42.30 p.2212).
    constexpr uintptr_t CONSOLE_WIN_BASE = mmap::SCI6;
    constexpr size_t CONSOLE_WIN_SIZE = 16u;

    static_assert(sci::SCR >= CONSOLE_WIN_BASE and sci::SCR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and sci::SMR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and sci::BRR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and sci::SCMR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and sci::SEMR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and sci::SNFR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and sci::SIMR1 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and sci::SIMR2 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and sci::SIMR3 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and sci::SPMR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE,
                  "arch_console_reclaim writes outside the window it reports");

    void unlock_registers(bool on)
    {
        if (on)
        {
            r16(cgc::PRCR) = cgc::PRCR_UNLOCK;
        }
        else
        {
            r16(cgc::PRCR) = cgc::PRCR_LOCK;
        }
    }

    // A PmnPFS write lands ONLY inside this bracket (UM sec.23.2.1): B0WI must be
    // cleared before PFSWE is writable, so the unlock is two writes, not one. Without
    // it the PFS write is dropped silently.
    void mpc_pfs_unlock(bool on)
    {
        if (on)
        {
            r8(mpc::PWPR) = 0x00;
            r8(mpc::PWPR) = mpc::PWPR_PFSWE;
        }
        else
        {
            r8(mpc::PWPR) = mpc::PWPR_B0WI;
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
    // reset clock) if the oscillator or PLL never reports stable. Must run inside the
    // PRCR unlock (clock regs are PRC0).
    bool clock_to_pll_240mhz()
    {
        r8(cgc::MOFCR) = cgc::MOFCR_XTAL_24MHZ;    // 1. crystal drive range
        r8(cgc::MOSCWTCR) = cgc::MOSCWTCR_MSTS;    // 2. oscillation settling count
        r8(cgc::MOSCCR) = cgc::MOSCCR_MAIN_RUN;    // 3. start the main clock oscillator
        uint8_t mosccr_rb = r8(cgc::MOSCCR);       // read back before dependent writes (sec.9.2.8)
        (void)mosccr_rb;
        if (not poll_flag(cgc::OSCOVFSR, cgc::OSCOVFSR_MOOVF, CLOCK_POLL_LIMIT))
        {
            return false;
        }
        // ICLK will exceed 120 MHz, so the code flash needs one wait state; set it
        // BEFORE running the PLL, per the sec.9.8 case (1) step order (step 4, ahead of
        // PLLCR). Read back so it is in effect before ICLK can rise. MEMWAIT=1 is
        // legal at any ICLK (Table 9.3), so it is harmless if the PLL never locks.
        r8(cgc::MEMWAIT) = cgc::MEMWAIT_ONE_WAIT;  // 4. one wait state (>120 MHz)
        uint8_t memwait_rb = r8(cgc::MEMWAIT);
        (void)memwait_rb;
        r16(cgc::PLLCR) = cgc::PLLCR_PLL_240MHZ;   // 5. multiplier + input divider
        r8(cgc::PLLCR2) = cgc::PLLCR2_PLL_RUN;     // 6. run the PLL
        if (not poll_flag(cgc::OSCOVFSR, cgc::OSCOVFSR_PLOVF, CLOCK_POLL_LIMIT))
        {
            return false;
        }
        // Set the dividers BEFORE the source switch: while still on the LOCO the
        // divisors apply to ~240 kHz, so no peripheral overshoots when the PLL
        // becomes the source on the next write.
        r32(cgc::SCKCR) = cgc::SCKCR_240MHZ;
        r16(cgc::SCKCR3) = cgc::SCKCR3_CKSEL_PLL;  // 8. ICLK <- PLL
        return true;
    }

    // HOCO frequency, from HOCOCR2.HCFRQ (UM sec.9.2.13 p.364). Readable whatever
    // HOCOCR.HCSTP says; only the WRITE is gated on the HOCO being stopped.
    uint32_t hoco_hz()
    {
        uint8_t const f = r8(cgc::HOCOCR2) & cgc::HOCOCR2_HCFRQ_MASK;
        if (f == 0)
        {
            return cgc::HOCO_16MHZ;
        }
        if (f == 1)
        {
            return cgc::HOCO_18MHZ;
        }
        if (f == 2)
        {
            return cgc::HOCO_20MHZ;
        }
        return 0; // 11b is prohibited: no frequency to report
    }

    // The clock the SCKCR dividers are fed from, read out of the LIVE tree rather than
    // assumed from what clock_to_pll_240mhz() intended, so a degraded bring-up (or a later
    // retune) is reflected instead of silently mispriced. Returns 0 for a tree this cannot
    // describe, which every caller must forward rather than substitute a nominal for.
    uint32_t clock_source_hz()
    {
        uint16_t const sel =
            (r16(cgc::SCKCR3) >> cgc::SCKCR3_CKSEL_S) & cgc::SCKCR3_CKSEL_MASK;
        if (sel == cgc::CKSEL_LOCO)
        {
            return cgc::LOCO_HZ;
        }
        if (sel == cgc::CKSEL_HOCO)
        {
            return hoco_hz();
        }
        if (sel == cgc::CKSEL_MAIN)
        {
            return cgc::MAIN_OSC_HZ;
        }
        if (sel == cgc::CKSEL_SUB)
        {
            return cgc::SUB_OSC_HZ;
        }
        if (sel != cgc::CKSEL_PLL)
        {
            return 0; // 101..111 are prohibited encodings
        }
        uint16_t const pllcr = r16(cgc::PLLCR);
        uint32_t in = cgc::MAIN_OSC_HZ;
        if ((pllcr & cgc::PLLCR_PLLSRCSEL) != 0)
        {
            in = hoco_hz();
        }
        uint32_t const plidiv = (pllcr & cgc::PLLCR_PLIDIV_MASK) + 1u;
        uint32_t const stc = (pllcr >> cgc::PLLCR_STC_S) & cgc::PLLCR_STC_MASK;
        if (in == 0u or plidiv > 3u or stc < cgc::PLLCR_STC_MIN or stc > cgc::PLLCR_STC_MAX)
        {
            return 0;
        }
        // The multiplier runs in HALF steps, so it is applied as (STC+1)/2 with the divide
        // last: rounding it to an integer first would drop 500 kHz per PLL input MHz.
        uint64_t const out = static_cast<uint64_t>(in) * (stc + 1u) / (2u * plidiv);
        return static_cast<uint32_t>(out);
    }

    // PCLKB, the SCI and CMTW clock (UM sec.42 preamble p.2144 for the SCI).
    uint32_t pclkb_hz()
    {
        uint32_t const src = clock_source_hz();
        uint32_t const pckb = (r32(cgc::SCKCR) >> cgc::SCKCR_PCKB_S) & cgc::SCKCR_DIV_MASK;
        if (src == 0u or pckb > cgc::SCKCR_DIV_MAX)
        {
            return 0; // 0111..1111 are prohibited division ratios
        }
        return src >> pckb;
    }

    // Enable the 8 KB flash ROM cache (UM sec.64.7.1). Reset auto-invalidates it, so
    // there is no coherency risk today; a future flash self-program must re-invalidate
    // (write ROMCIV=1) before re-enable. Bounded invalidate poll degrades, never hangs.
    void rom_cache_enable()
    {
        r16(flash::ROMCIV) = flash::ROMCIV_ROMCIV;
        for (uint32_t i = 0; i < CLOCK_POLL_LIMIT; i++)
        {
            if ((r16(flash::ROMCIV) & flash::ROMCIV_ROMCIV) == 0)
            {
                break;
            }
        }
        r16(flash::ROMCE) = flash::ROMCE_ROMCEN;
    }

    void sci6_console_init()
    {
        // Pin mux: route PB1->TXD6, PB0->RXD6 (UM sec.23.4.1 procedure). PSEL is only
        // writable while the pin's PMR bit is 0, which it is at reset. Without the PMR
        // step the pins stay GPIO.
        mpc_pfs_unlock(true);
        r8(mpc::PB1PFS) = mpc::PFS_PSEL_SCI6; // TXD6
        r8(mpc::PB0PFS) = mpc::PFS_PSEL_SCI6; // RXD6
        mpc_pfs_unlock(false);
        r8(port::PORTB_PMR) |= port::PB1 | port::PB0; // PB1,PB0 -> peripheral function

        // SMR, SCMR, SEMR and BRR are all writable only with TE and RE both 0
        // (UM sec.42.2.9/12/13/15 notes).
        r8(sci::SCR) = 0;
        r8(sci::SCMR) = sci::SCMR_UART;

        // The divisor comes from the LIVE clock tree, so a bring-up that fell back to the
        // LOCO still lands a usable console instead of a rate off by three orders of
        // magnitude. A tree pclkb_hz() cannot describe leaves the reset divisor alone.
        sci::BaudSetting bs;
        if (sci::baud_select(pclkb_hz(), CONSOLE_BAUD, &bs))
        {
            r8(sci::SMR) = bs.cks; // async, 8 data bits, no parity, one stop bit
            r8(sci::SEMR) = bs.semr;
            r8(sci::BRR) = bs.brr;
        }
        // Volatile so -Os keeps the wait. UM sec.42 requires NO settle here: Fig.42.13
        // p.2234 has the hardware itself hold TXD high for one frame after TE goes 1.
        for (volatile uint32_t d = 0; d < 10000u;)
        {
            d = d + 1;
        }
        // TIE stays off here: "a TXI interrupt request is not generated by ... setting the
        // SCR.TIE bit to 1 while the setting of the SCR.TE bit is 1" (UM sec.42.12.2(1)
        // p.2308), so the pass that arms TIE must also write the first byte or nothing ever
        // fires. The ring's idle -> busy prime does both.
        r8(sci::SCR) = sci::SCR_TE;
    }

    int rx_tx_slot_free(void) { return (r8(sci::SSR) & sci::SSR_TDRE) != 0; }
    void rx_tx_push(uint8_t b) { r8(sci::TDR) = b; }
    void rx_tx_irq_enable(void) { r8(sci::SCR) = static_cast<uint8_t>(r8(sci::SCR) | sci::SCR_TIE); }
    void rx_tx_irq_disable(void) { r8(sci::SCR) = static_cast<uint8_t>(r8(sci::SCR) & ~sci::SCR_TIE); }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const rx_console_backend = {
        rx_tx_slot_free, rx_tx_push, rx_tx_irq_enable, rx_tx_irq_disable};

    // One row per group, indexed by the group_index of the logical-line arithmetic in
    // <kickos/arch/rx_group.h>. The ORDER IS THE ABI: it fixes TEI6 = 268 and ERI6 = 269
    // (group 0 = GROUPBL0, bits 12 and 13), so inserting a row renumbers every line above.
    struct group_desc
    {
        uintptr_t status; // GRPxxx, read-only
        uintptr_t enable; // GENxxx
        int vector;       // the group's own ICU vector
    };
    constexpr group_desc GROUPS[] = {
        {icu::GRPBL0, icu::GENBL0, icu::GROUPBL0_VECTOR},
        {icu::GRPBL1, icu::GENBL1, icu::GROUPBL1_VECTOR},
        {icu::GRPBL2, icu::GENBL2, icu::GROUPBL2_VECTOR},
        {icu::GRPAL0, icu::GENAL0, icu::GROUPAL0_VECTOR},
        {icu::GRPAL1, icu::GENAL1, icu::GROUPAL1_VECTOR},
    };
    constexpr unsigned GROUP_COUNT = sizeof(GROUPS) / sizeof(GROUPS[0]);
    static_assert(kickos::rxv3::GROUP_LINE_STRIDE == 32,
                  "a group register holds exactly 32 sources (ISj/ENj, j = 0..31)");
}

extern "C"
{

void arch_init(void)
{
    unlock_registers(true);
    (void)clock_to_pll_240mhz(); // the landed tree is read back below, not assumed here
    // Release the module stops for the timer + console (UM sec.11 MSTPCR).
    r32(cgc::MSTPCRA) &= ~(cgc::MSTPA_CMTW0 | cgc::MSTPA_CMTW1);
    r32(cgc::MSTPCRB) &= ~cgc::MSTPB_SCI6;
    unlock_registers(false);

    rom_cache_enable(); // MEMWAIT set above; ROM cache is not PRCR-gated

    // Read back, not assumed from on_pll: a bring-up that degraded to the LOCO must price
    // its ticks and its baud at the rate it actually landed on. A tree the derivation
    // cannot describe leaves both at their reset nominals rather than at zero.
    uint32_t const src = clock_source_hz();
    uint32_t const ick = (r32(cgc::SCKCR) >> cgc::SCKCR_ICK_S) & cgc::SCKCR_DIV_MASK;
    if (src != 0u and ick <= cgc::SCKCR_DIV_MAX)
    {
        SystemCoreClock = src >> ick;
    }
    uint32_t const pclkb = pclkb_hz();
    if (pclkb != 0u)
    {
        kickos_rx_timer_hz = pclkb / 8u; // CMTW input is PCLKB/8
    }

    sci6_console_init();

    // Timer line (CMTW0 compare match, vector 30): priority below the kernel lock
    // level, then enable at the ICU. (The CMTW's own CMWIE is set per-arm.)
    r8(icu::IPR006) = 4; // IPL_DEVICE (< IPL_LOCK)
    r8(icu::IER03) |= icu::IER03_CMWI0;

    kickos_rxv3_init(); // start CMTW1 free-run + reset arch software state
}

// Arch seam (arch_rxv3.cc): arm or disarm ONE group source, and the group vector itself.
//
// The group vector is armed lazily, and must be: nothing reaches the CPU unless it is
// enabled, yet it is not a claimable line (owning "the group" would starve every other
// source in it), so no line's own mask can arm it. Arming every group vector at init
// leaves a first-level ISR reachable with the status word zero, which on a LEVEL input is
// a silent storm rather than a diagnosable fault.
//
// GENxxx is the only record of which sources are armed; there is no shadow counter for
// repeated masks and unmasks to desynchronize (the tier-1 rearm path unmasks on every wait).
//
// Both orderings follow UM sec.15.7.1/15.7.2 p.545: ENj before IERm.IENj when arming,
// IERm.IENj before ENj when disarming.
void kickos_rx_group_arm(int line, int on)
{
    unsigned const slot = static_cast<unsigned>(line - kickos::rxv3::GROUP_LINE_BASE);
    unsigned const g = slot / static_cast<unsigned>(kickos::rxv3::GROUP_LINE_STRIDE);
    unsigned const bit = slot % static_cast<unsigned>(kickos::rxv3::GROUP_LINE_STRIDE);
    if (g >= GROUP_COUNT)
    {
        return; // no such group on this chip: the line can never fire, so never arm it
    }
    // Self-bracketed like the core's icu_ier_set: GENxxx packs 32 sources into one word,
    // so this RMW must be atomic against a device ISR masking a sibling source in the
    // same group, whatever the caller holds.
    arch_irq_state_t const s = arch_irq_save();
    uint32_t const en = r32(GROUPS[g].enable);
    uint32_t const mask = 1u << bit;
    if (on != 0)
    {
        r32(GROUPS[g].enable) = en | mask;
        if (en == 0u)
        {
            arch_irq_unmask(GROUPS[g].vector); // IPR + IER for the group vector itself
        }
    }
    else
    {
        uint32_t const next = en & ~mask;
        if (next == 0u)
        {
            arch_irq_mask(GROUPS[g].vector);
        }
        r32(GROUPS[g].enable) = next;
    }
    arch_irq_restore(s);
}

// Arch seam (arch_rxv3.cc): the chip's device demux, called from the shared first-level
// ISR. Only group vectors route here; every dedicated source (CMWI0, the two SCI6 vectors,
// both software interrupts) has its own INTB slot.
//
// One group assertion can carry several ISj bits (UM sec.15.5.4 Fig.15.17 p.542), hence
// the loop.
//
// The status word must be SNAPSHOTTED before the first post: kickos_isr_irq runs
// irq_event_isr, which masks the source at GENxxx.ENj, and clearing ENj also clears that
// source's ISj (UM sec.15.2.24(3) p.504), so re-reading GRPxxx between posts loses the
// sources not yet dispatched.
//
// No ICU.IR write anywhere: for a level source the IR flag must not be written at all (UM
// sec.15.2.1(2) p.480). It goes down on its own once the driver clears the peripheral flag
// or the mask clears ENj.
void kickos_rx_dev_dispatch(void)
{
    for (unsigned g = 0; g < GROUP_COUNT; g++)
    {
        uint32_t const status = r32(GROUPS[g].status);
        if (status == 0u)
        {
            continue;
        }
        for (unsigned bit = 0; bit < 32u; bit++)
        {
            if ((status & (1u << bit)) == 0u)
            {
                continue;
            }
            kickos_isr_irq(kickos::rxv3::GROUP_LINE_BASE
                           + static_cast<int>(g * 32u + bit));
        }
    }
}

// Branch-clock oracle (arch.h): report the clock feeding a peripheral block so a userspace
// driver derives its own divisor. SCI0..SCI6 run on PCLKB (UM sec.42 preamble p.2144),
// which is READ OUT OF THE LIVE TREE on every call rather than cached from arch_init: the
// whole point is that a driver's reported baud tracks the clock the channel is actually on.
//
// The SYSTEM block this reads is kernel-reserved (arch_reserved_blocks), so the holder of
// an SCI window has no way to read the select itself. A block this chip does not model
// returns 0, which the contract makes the caller refuse on rather than guess.
//
// This definition MUST stay in this TU: the member is always anchored (arch_init lives
// here and the kernel references it; RX gets no -u force-ref), and a dedicated TU nothing
// else references would leave the arch/common fallback answering with 0 and no link error
// at all.
uint32_t arch_periph_clock_hz(uintptr_t base)
{
    if (base == mmap::SCI6)
    {
        return pclkb_hz();
    }
    // RIICa is on PCLKB too (UM sec.43 preamble, "PCLK" there being PCLKB).
    if (base == mmap::RIIC0 or base == mmap::RIIC1 or base == mmap::RIIC2)
    {
        return pclkb_hz();
    }
    return 0;
}

// Ungate a block for the unprivileged thread that already holds its window (arch.h). A RIIC
// channel comes out of reset in module stop: its registers read back reset values and drop
// stores until MSTPCR says otherwise (UM sec.43.16.1), and MSTPCRB/MSTPCRC sit in the
// kernel-reserved SYSTEM block behind PRCR.PRC1, out of the window holder's reach.
//
// The console and timer blocks are deliberately absent: arch_init releases those, and
// answering for them would let a grant holder gate a block the kernel is using.
//
// Same must-stay-in-this-TU rule as arch_periph_clock_hz above.
int arch_periph_enable(uintptr_t base)
{
    uintptr_t reg = 0;
    uint32_t bit = 0;

    if (base == mmap::RIIC0)
    {
        reg = cgc::MSTPCRB;
        bit = cgc::MSTPB_RIIC0;
    }
    else if (base == mmap::RIIC1)
    {
        reg = cgc::MSTPCRB;
        bit = cgc::MSTPB_RIIC1;
    }
    else if (base == mmap::RIIC2)
    {
        reg = cgc::MSTPCRC;
        bit = cgc::MSTPC_RIIC2;
    }
    else
    {
        return -KOS_EINVAL;
    }

    unlock_registers(true);
    r32(reg) &= ~bit;
    unlock_registers(false);
    if ((r32(reg) & bit) != 0u)
    {
        return -KOS_EPERM; // the protect bracket did not take
    }
    return 0;
}

void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n); // buffered; the routing guard (console.cc) keeps this thread-only
}

// Bounded polled writer: panic / fault / pre-arm boot route here (console.cc). Must
// stay reachable with the scheduler and IRQs down: no ring, no interrupt.
void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((r8(sci::SSR) & sci::SSR_TDRE) == 0)
        {
            if (++spin >= CONSOLE_POLL_LIMIT)
            {
                return; // TDRE never cleared (SCI dead/misconfigured): drop, don't hang
            }
        }
        r8(sci::TDR) = static_cast<uint8_t>(buf[i]);
    }
}

// Block until the shift register is idle, not merely the holding register: SSR.TEND is set
// only when a character's tail-end bit goes out with TDR not reloaded, and clearing SCR.TE
// also sets it (UM sec.42.2.11 p.2171).
void arch_console_flush_sync(void)
{
    // A wedged SCI costs the tail of a line, not a hang.
    (void)poll_flag(sci::SSR, sci::SSR_TEND, CONSOLE_POLL_LIMIT);
}

// Arch seam (console_tx.h): hand the kernel the SCI6 backend + ring storage + the
// TXI6 line. console_buffer_init binds the drain ISR, unmasks the line (IPR087 +
// IER0A.IEN7 via arch_irq_unmask), and arms the ring.
console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = irq::SCI6_TXI;
    return &rx_console_backend;
}

void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Panic-path reclaim: force SCI6 back to a polled-ready 8N1 TX channel after a driver may
// have garbled every writable register in its granted window. Must be re-entrant from a
// nested fault, so it is straight-line ABSOLUTE stores only, never a read-modify-write.
// SCR.CKE = 1x points the transfer clock at the unwired SCK6 pin and starves the shifter
// (UM sec.42.2.10 p.2167 Note 2); both SCR stores here put CKE back to 00b.
void arch_console_reclaim(void)
{
    // SMR, SCMR, SEMR, BRR, SNFR, SIMR1 and SPMR are writable ONLY with TE and RE both 0
    // (notes on UM sec.42.2.9/.12/.13/.15/.16/.17/.21), so nothing below lands unless this
    // store comes first.
    r8(sci::SCR) = 0;

    // Ahead of SMR, because SMIF picks SMR's field layout.
    r8(sci::SCMR) = sci::SCMR_UART;

    // IICM = 0 leaves simple I2C mode, in which the TXD6/SSDA6/SMOSI6 pin frames I2C data
    // rather than an async character (UM sec.42.2.17 p.2199); IICDL = 0.
    r8(sci::SIMR1) = 0;
    r8(sci::SIMR2) = 0; // I2C interrupt mode, clock synchronization, ACK transmission data

    // IICSDAS = 00b restores serial data output on the pin: 10b PINS IT LOW and 11b puts it
    // in high impedance (UM sec.42.2.19 p.2201).
    r8(sci::SIMR3) = 0;

    // CTSE set gates every byte on a CTS input this board does not wire (UM sec.42.2.21
    // p.2204). SSE, MSS, CKPOL and CKPH = 0 leave simple SPI mode.
    r8(sci::SPMR) = 0;

    // NFCS = 000b, the asynchronous-mode setting (UM sec.42.2.16 p.2198).
    r8(sci::SNFR) = 0;

    // Priced off the LIVE clock tree, NOT folded to a 60 MHz constant: clock_to_pll_240mhz
    // can fail and leave the part on the ~240 kHz LOCO.
    sci::BaudSetting bs;
    if (not sci::baud_select(pclkb_hz(), CONSOLE_BAUD, &bs))
    {
        // A clock tree baud_select cannot price gets the reset triple.
        bs.cks = 0;
        bs.semr = 0;
        bs.brr = sci::BRR_RESET;
    }
    r8(sci::SMR) = bs.cks;   // CM 0 async, CHR 0 for 8 bits with SCMR.CHR1, PE/PM/STOP/MP 0
    r8(sci::SEMR) = bs.semr; // NFEN, RXDESEL, ABCSE, ACS0 and BRME all 0
    r8(sci::BRR) = bs.brr;

    // TX only, TIE off, and LAST.
    r8(sci::SCR) = sci::SCR_TE;
}

void arch_diag_led_init(void)
{
    r8(port::PORT8_PMR) &= ~port::LED6;   // GPIO (not peripheral)
    r8(port::PORT8_PODR) |= port::LED6;   // drive high => LED off (active-low, board Table 5-9)
    r8(port::PORT8_PDR) |= port::LED6;    // output
}

void arch_diag_led_set(int on)
{
    if (on != 0)
    {
        r8(port::PORT8_PODR) &= ~port::LED6; // low => LED on
    }
    else
    {
        r8(port::PORT8_PODR) |= port::LED6;  // high => LED off
    }
}

// Kernel-owned pins arch_pinmux_set refuses so a board map or an app cannot dark the
// console: PB1/TXD6 + PB0/RXD6, muxed for life by sci6_console_init.
static bool rx72m_pin_kernel_owned(uint32_t p, uint32_t pin)
{
    return p == 0x0Bu and pin <= 1u;
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET), covering BOTH mux stages an RX
// pin passes: the MPC PmnPFS peripheral-function select and the PORTm.PMR general-I/O
// vs peripheral switch. Mediating PMR alone would leave the refusal above bypassable:
// PSEL can re-point a pin already at PMR=1 (the console pins are) at a different module
// without PMR ever being written. func packs both stages, encoded chip-locally
// (reg::mpc::PINMUX_*).
int arch_pinmux_set(uint32_t p, uint32_t pin, uint32_t func)
{
    if (p > port::PORT_INDEX_MAX or pin > port::PIN_MAX)
    {
        return -KOS_EINVAL;
    }
    if ((func & mpc::PINMUX_RESERVED) != 0u)
    {
        return -KOS_EINVAL;
    }
    if (rx72m_pin_kernel_owned(p, pin))
    {
        return -KOS_EBUSY;
    }
    uintptr_t const pmr = port::pmr(p);
    uint8_t const bit = static_cast<uint8_t>(1u << pin);
    if ((func & mpc::PINMUX_PFS_EN) != 0u)
    {
        // PSEL may only change while this pin's PMR bit is 0, else the pin emits an
        // unexpected edge (UM sec.23.4.2 (1)). Clearing it first is step 1 of sec.23.4.1.
        r8(pmr) = static_cast<uint8_t>(r8(pmr) & ~bit);
        mpc_pfs_unlock(true);
        r8(mpc::pfs(p, pin)) = static_cast<uint8_t>(func & mpc::PINMUX_PFS_MASK);
        mpc_pfs_unlock(false);
    }
    if ((func & mpc::PINMUX_PMR) != 0u)
    {
        r8(pmr) = static_cast<uint8_t>(r8(pmr) | bit);
    }
    else
    {
        r8(pmr) = static_cast<uint8_t>(r8(pmr) & ~bit);
    }
    return 0;
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

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (RX72M UM). Owns-for-life: the CMTW time base (CMTW0 @0x94200
// timebase and CMTW1 @0x94280 bench/trace clock fit one 0x100 block), the ICU (the RX IRQ
// controller is MPU-GOVERNED memory, unlike the ARM PPB, so it must be reserved), the
// bus-side MPU register file, and the SYSTEM clock/reset gate block.
//
// The ICU window must reach past GENAL1 @0x87874, not stop at IR/IER/IPR: short of that,
// an AUTH_MEMORY holder could be granted GENBL0 and arm or disarm any group source behind
// the kernel's back.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {mmap::CMTW0, 0x100u}, // CMTW0 + CMTW1: time base + bench clock (UM sec.32)
        {mmap::ICU, icu::SPAN}, // ICU: IR + IER + IPR + the group registers (UM sec.15)
        {mmap::MPU, 0x140u},   // MPU: RSPAGE/REPAGE + MPEN/MPBAC/MPOPI register file (UM sec.17)
        {mmap::SYSTEM, 0x100u}, // SYSTEM: MSTPCR / SCKCR / PLLCR clock+reset gates (UM sec.9/11)
    };
    size_t n = sizeof(blocks) / sizeof(blocks[0]);
    if (n > max)
    {
        n = max;
    }
    for (size_t i = 0; i < n; i++)
    {
        out[i] = blocks[i];
    }
    return n;
}
#endif

// C runtime init, reached from _start (startup.S). Never returns.
void rx_reset_handler(void)
{
    kickos_ranges_init(); // init .data + the pow2 app-data block; zero .bss + app-bss
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
