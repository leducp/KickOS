// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Atmel/Microchip AT91SAM3X8E (Arduino Due, Cortex-M3) chip backend. Registers
// clean-room from the SAM3X/SAM3A datasheet (Atmel-11057); hand-rolled, no ASF.
//
// clock_init() brings the part up on the
// 12 MHz crystal + PLLA to MCK = 84 MHz (SAM3X max); the core boots on the
// imprecise 4 MHz fast RC, at which 115200 is unreachable.

#include <kickos/arch/arch.h>
#include <kickos/config/limits.h>
#include <kickos/arch/clk_anchor.h> // shared tickless-clock epoch anchor (B2)
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h> // KOS_E* codes for arch_pinmux_set


#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

namespace
{
    // The two MAINCK sources. Every rate this file states is one of these two through the
    // PLLA multiply and the MCKR prescaler in clock_init, and never a second spelling of a
    // product. Ahead of SystemCoreClock because its initialiser is the rate at reset.
    constexpr uint32_t MAINCK_RC_HZ = 4000000u;
    constexpr uint32_t MAINCK_XTAL_HZ = 12000000u;
}

extern "C"
{
    void kickos_armv7m_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // PMC_MCKR comes out of reset selecting MAINCK undivided (sec.28.15.11, reset 0x1) and
    // MAINCK is the fast RC, so MCK is this until clock_init moves one of the two terms.
    uint32_t SystemCoreClock = MAINCK_RC_HZ;
}

namespace
{

    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    constexpr uintptr_t WDT_MR = 0x400E1A54;    // write-once; WDDIS = bit 15
    constexpr uint32_t WDT_MR_WDDIS = 1u << 15;
    constexpr uintptr_t SCB_VTOR = 0xE000ED08;
    constexpr uintptr_t FLASH_BASE = 0x00080000; // real flash (aliased at 0x0)

    // EEFC (sec.18): the two flash banks. FWS (EEFC_FMR bits 11:8) sets the flash
    // read/write wait states; per sec.45 the AC-flash table, FWS=4 (5 read cycles)
    // covers up to 90 MHz at VDDCORE 1.8V, required for 84 MHz. Set BEFORE the
    // clock is raised. EEFC_FMR at 0x400E0A00 (bank 0) / 0x400E0C00 (bank 1).
    constexpr uintptr_t EEFC0_FMR = 0x400E0A00;
    constexpr uintptr_t EEFC1_FMR = 0x400E0C00;
    constexpr uint32_t FMR_FWS_4 = 4u << 8;

    // PMC (sec.28): clock generator + status. Base 0x400E0600.
    constexpr uintptr_t PMC_BASE = 0x400E0600;
    constexpr uintptr_t CKGR_MOR = PMC_BASE + 0x20;   // Main Oscillator Register
    constexpr uintptr_t CKGR_PLLAR = PMC_BASE + 0x28; // PLLA Register
    constexpr uintptr_t PMC_MCKR = PMC_BASE + 0x30;   // Master Clock Register
    constexpr uintptr_t PMC_SR = PMC_BASE + 0x68;     // Status Register

    // CKGR_MOR (sec.28): crystal oscillator. KEY 0x37 (bits 23:16) gates the write;
    // MOSCXTST (15:8) is the crystal startup counter (in SLCK/8); keep the fast RC
    // (MOSCRCEN) enabled while the crystal warms up, then MOSCSEL picks the crystal.
    //
    // MOSCXTS asserts when the MOSCXTST counter expires, NOT on physical crystal
    // detection, so MOSCXTST MUST cover the crystal's worst-case warm-up or the
    // status lies and PLLA locks on a still-settling MAINCK (the intermittent-boot
    // race). SLCK is the on-chip slow RC, spec'd 22..42 kHz, so size the count at
    // the FAST end. Time = MOSCXTST * 8 / SLCK. 0x80 = 128 -> 24.4 ms @42 kHz,
    // 31.2 ms @32 kHz, 46.5 ms @22 kHz, comfortably past the ~15 ms crystal spec.
    // GUESS pending Due silicon: confirm the crystal's startup spec before trimming
    // toward the ~15 ms figure (0x50 gives ~15.2 ms @42 kHz, no margin).
    constexpr uint32_t MOR_KEY = 0x37u << 16;
    constexpr uint32_t MOR_MOSCXTEN = 1u << 0;
    constexpr uint32_t MOR_MOSCRCEN = 1u << 3;
    constexpr uint32_t MOR_MOSCXTST = 0x80u << 8;
    constexpr uint32_t MOR_MOSCSEL = 1u << 24;
    constexpr uint32_t MOR_CRYSTAL = MOR_KEY | MOR_MOSCXTST | MOR_MOSCRCEN | MOR_MOSCXTEN;

    // CKGR_PLLAR (sec.28): PLLA = MAINCK * (MULA+1) / DIVA. ONE (bit 29) reads 1;
    // PLLCOUNT (13:8) = LOCK delay in SLCK. MULA (26:16) and DIVA (7:0) are the register
    // spellings of the two figures beside them, so the rate cannot drift from the register.
    constexpr uint32_t PLLA_MUL = 14u;
    constexpr uint32_t PLLA_DIV = 1u;
    constexpr uint32_t PLLA_HZ = MAINCK_XTAL_HZ * PLLA_MUL / PLLA_DIV;
    constexpr uint32_t PLLAR_ONE = 1u << 29;
    constexpr uint32_t PLLAR_MULA = (PLLA_MUL - 1u) << 16;
    constexpr uint32_t PLLAR_COUNT = 0x3Fu << 8;
    constexpr uint32_t PLLAR_DIVA = PLLA_DIV << 0;

    // PMC_MCKR (sec.28.15.11): CSS (1:0) source, PRES (6:4) prescaler.
    constexpr uint32_t MCKR_CSS_MASK = 3u << 0;
    constexpr uint32_t MCKR_CSS_MAIN = 1u << 0;
    constexpr uint32_t MCKR_CSS_PLLA = 2u << 0;
    constexpr uint32_t MCKR_PRES_MASK = 7u << 4;
    constexpr uint32_t MCKR_PRES_DIV2 = 1u << 4;

    // The three words this backend ever writes to PMC_MCKR.
    constexpr uint32_t MCKR_MAIN = MCKR_CSS_MAIN;
    constexpr uint32_t MCKR_MAIN_DIV2 = MCKR_PRES_DIV2 | MCKR_CSS_MAIN;
    constexpr uint32_t MCKR_PLLA_DIV2 = MCKR_PRES_DIV2 | MCKR_CSS_PLLA;

    // MCK from a PMC_MCKR word. PRES DIVIDES WHATEVER CSS SELECTED, so PRES_DIV2 beside
    // CSS_MAIN is the crystal halved and not PLLA halved. Every store of SystemCoreClock
    // below goes through this on the same word it just wrote, so the rate and the register
    // cannot be moved apart. A CSS this backend never writes answers 0, which a divisor
    // cannot mistake for a working clock.
    constexpr uint32_t mck_for(uint32_t mckr, uint32_t mainck)
    {
        uint32_t src = 0u;
        if ((mckr & MCKR_CSS_MASK) == MCKR_CSS_MAIN)
        {
            src = mainck;
        }
        else if ((mckr & MCKR_CSS_MASK) == MCKR_CSS_PLLA)
        {
            src = PLLA_HZ;
        }
        uint32_t div = 1u;
        if ((mckr & MCKR_PRES_MASK) == MCKR_PRES_DIV2)
        {
            div = 2u;
        }
        return src / div;
    }

    // The part's own maximum, and the bound FWS=4 was chosen against (sec.45 covers 90 MHz).
    constexpr uint32_t MCK_MAX_HZ = 84000000u;

    // The datasheet's own reading of the three words, checked against the derivation rather
    // than restated at the stores. The middle one is the whole reason this helper exists.
    static_assert(mck_for(MCKR_MAIN, MAINCK_XTAL_HZ) == MAINCK_XTAL_HZ,
                  "CSS_MAIN with no PRES is MAINCK undivided");
    static_assert(mck_for(MCKR_MAIN_DIV2, MAINCK_XTAL_HZ) == 6000000u,
                  "PRES_DIV2 beside CSS_MAIN halves the CRYSTAL: the intermediate state of "
                  "the PLL switch is 6 MHz, not the 12 MHz the crystal runs at");
    static_assert(mck_for(MCKR_PLLA_DIV2, MAINCK_XTAL_HZ) == MCK_MAX_HZ,
                  "PLLA over the MCKR prescaler must land on the SAM3X8E maximum MCK; the "
                  "flash wait states are sized for that bound too");

    // Selects a master clock and records the rate that selection runs at. ONE word for both,
    // so PMC_MCKR and SystemCoreClock cannot come to name different selections.
    void mckr_select(uint32_t mckr, uint32_t mainck);

    // PMC_SR (sec.28) poll bits.
    constexpr uint32_t SR_MOSCXTS = 1u << 0;   // crystal oscillator stable
    constexpr uint32_t SR_LOCKA = 1u << 1;     // PLLA locked
    constexpr uint32_t SR_MCKRDY = 1u << 3;    // master clock ready
    constexpr uint32_t SR_MOSCSELS = 1u << 16; // main oscillator selection done

    // Bounded poll; true iff the bit set before the bound expired. The bound is a
    // raw spin count on the reset 4 MHz RC: ~1M iterations is hundreds of ms, well
    // past the MOSCXTST window (tens of ms) and every SLCK-counted status delay,
    // so a good crystal always returns true. A false return means the source never
    // came up: the caller MUST NOT proceed to select it (that is the boot race).
    bool pmc_wait(uint32_t bit)
    {
        for (uint32_t i = 0; i < 0x100000u; i++)
        {
            if ((r32(PMC_SR) & bit) != 0)
            {
                return true;
            }
        }
        return false;
    }

    // NO DEGRADE RETURN CARRIES A RATE OF ITS OWN. SystemCoreClock is restated at every edge
    // that moves MCK and every failure below just returns, so a step added or reordered
    // cannot leave a stale figure behind for uart_init's divisor to follow. PMC_MCKR comes
    // out of reset selecting MAINCK undivided (sec.28.15.11, reset 0x1), which is what makes
    // the initialiser above the rate at entry and what makes step 3's MOSCSEL move MCK with
    // no PMC_MCKR write of its own.
    void clock_init()
    {
        // 1. Flash wait states first, both banks (sec.18 / sec.45), before raising
        //    MCK. Over-provisioning is safe: FWS=4 also covers the RC/main fallbacks.
        r32(EEFC0_FMR) = FMR_FWS_4;
        r32(EEFC1_FMR) = FMR_FWS_4;

        // 2. Start the 12 MHz crystal (RC stays MAINCK meanwhile). If MOSCXTS never
        //    asserts there is no usable crystal: stay on the 4 MHz fast RC so the core
        //    and diag LED still run. uart_init derives the divisor from this rate, which
        //    4 MHz cannot divide to 115200: degraded console, not a dead-locked part.
        r32(CKGR_MOR) = MOR_CRYSTAL;
        if (not pmc_wait(SR_MOSCXTS))
        {
            return;
        }

        // 3. Select the crystal as MAINCK, then run MCK off it before touching PLLA. The
        //    MOSCSEL switch is glitch-free and signals on MOSCSELS, not MCKRDY (sec.27.5.3),
        //    and CSS already names MAINCK, so MCK is on the crystal before the write below.
        r32(CKGR_MOR) = MOR_CRYSTAL | MOR_MOSCSEL;
        if (not pmc_wait(SR_MOSCSELS))
        {
            // MOSCSEL moves MCK with no PMC_MCKR write, so a switch that landed without
            // reporting would leave MCK on the crystal while the rate below still reads the
            // RC. Deselect before returning, and the asserted rate is true either way.
            r32(CKGR_MOR) = MOR_CRYSTAL;
            return;
        }
        mckr_select(MCKR_MAIN, MAINCK_XTAL_HZ);
        if (not pmc_wait(SR_MCKRDY))
        {
            return;
        }

        // 4. PLLA (sec.28 CKGR_PLLAR). If it never locks, MCK is already stable on the
        //    crystal, so stay there (console still off: 12 MHz divides to 107143 baud,
        //    outside 8N1 tolerance).
        r32(CKGR_PLLAR) = PLLAR_ONE | PLLAR_MULA | PLLAR_COUNT | PLLAR_DIVA;
        if (not pmc_wait(SR_LOCKA))
        {
            return;
        }

        // 5. Switch MCK to PLLA/2. sec.28.12 mandates, for a PLL source: set PRES, wait
        //    MCKRDY, then set CSS, wait MCKRDY (two writes, not one). THE FIRST WRITE
        //    HALVES MAINCK, so the intermediate state is 6 MHz and not 12: PRES divides
        //    whatever CSS names, and CSS still names MAINCK here.
        mckr_select(MCKR_MAIN_DIV2, MAINCK_XTAL_HZ);
        if (not pmc_wait(SR_MCKRDY))
        {
            return;
        }
        mckr_select(MCKR_PLLA_DIV2, MAINCK_XTAL_HZ);
        if (not pmc_wait(SR_MCKRDY))
        {
            return;
        }
    }

    void mckr_select(uint32_t mckr, uint32_t mainck)
    {
        r32(PMC_MCKR) = mckr;
        SystemCoreClock = mck_for(mckr, mainck);
    }

    // PMC (sec.28): per-peripheral clock enable by peripheral ID.
    constexpr uintptr_t PMC_PCER0 = 0x400E0610;
    constexpr uint32_t PID_UART = 1u << 8;
    constexpr uint32_t PID_PIOA = 1u << 11;

    // PIOA (sec.31): route PA8/PA9 to the UART (peripheral A).
    constexpr uintptr_t PIOA_BASE = 0x400E0E00;
    constexpr uintptr_t PIOA_PDR = PIOA_BASE + 0x04; // give pins to the peripheral
    constexpr uint32_t PA8_PA9 = (1u << 8) | (1u << 9);

    // --- Pin-mux (KOS_SYS_PINMUX_SET) -------------------------------------------
    // One PIO controller per port: PIOA + port*0x200 (A=0..D=3). PMC_PCER0 clock
    // bit = (11+port) (PIOA is peripheral ID 11). func selects the routing:
    //   0x00 = GPIO output (PIO_PER + PIO_OER), 0x01 = GPIO input (PIO_PER + PIO_ODR),
    //   0x10 = peripheral A (ABSR bit CLEAR, then PIO_PDR),
    //   0x11 = peripheral B (ABSR bit SET,   then PIO_PDR).
    // The ABSR write MUST precede PDR (PDR hands the pin to whichever peripheral
    // ABSR currently selects). The OER/ODR write is MANDATORY: PER alone leaves the
    // output driver at its reset state, giving a dead output. PIO pull-ups are
    // enabled at reset (datasheet reset state); this backend does not touch PUER/PUDR.
    constexpr uintptr_t PIO_STRIDE = 0x200;
    constexpr uintptr_t PIO_PER_OFF = 0x00;
    constexpr uintptr_t PIO_PDR_OFF = 0x04;
    constexpr uintptr_t PIO_OER_OFF = 0x10;
    constexpr uintptr_t PIO_ODR_OFF = 0x14;
    constexpr uintptr_t PIO_ABSR_OFF = 0x70;
    constexpr uint32_t PMC_PID_PIO_SHIFT = 11u;
    constexpr uint32_t PINMUX_PORT_MAX = 3u; // PIOA..PIOD
    constexpr uint32_t PINMUX_FUNC_GPIO_OUT = 0x00u;
    constexpr uint32_t PINMUX_FUNC_GPIO_IN = 0x01u;
    constexpr uint32_t PINMUX_FUNC_PERIPH_A = 0x10u;
    constexpr uint32_t PINMUX_FUNC_PERIPH_B = 0x11u;

    // Kernel-owned pins arch_pinmux_set refuses so a board map cannot dark the
    // console or steal the diag LED. PA8/PA9 = console UART; PB27 = "L" LED.
    bool sam_pin_kernel_owned(uint32_t port, uint32_t pin)
    {
        return (port == 0u and (pin == 8u or pin == 9u)) or (port == 1u and pin == 27u);
    }

    // UART (sec.34), dedicated simple UART.
    constexpr uintptr_t UART_BASE = 0x400E0800;
    constexpr uintptr_t UART_CR = UART_BASE + 0x00;
    constexpr uintptr_t UART_MR = UART_BASE + 0x04;
    constexpr uintptr_t UART_IER = UART_BASE + 0x08; // interrupt enable (write 1 to set)
    constexpr uintptr_t UART_IDR = UART_BASE + 0x0C; // interrupt disable (write 1 to clear)
    constexpr uintptr_t UART_SR = UART_BASE + 0x14;
    constexpr uintptr_t UART_THR = UART_BASE + 0x1C;
    constexpr uintptr_t UART_BRGR = UART_BASE + 0x20;
    constexpr uint32_t CR_RSTRX_RSTTX = (1u << 2) | (1u << 3);
    constexpr uint32_t CR_RXEN_TXEN = (1u << 4) | (1u << 6);
    constexpr uint32_t MR_NO_PARITY = 4u << 9; // PAR=100 (none), CHMODE=normal
    constexpr uint32_t SR_TXRDY = 1u << 1;
    constexpr uint32_t IER_TXRDY = 1u << 1; // TXRDY bit in IER/IDR/IMR (same position as SR)
    constexpr uint32_t CONSOLE_BAUD = 115200u;

    // CD = MCK/(16*baud), rounded (sec.34). CD 0 stops the generator, so a clock too slow
    // to divide gets 1 rather than silence. This UART has no fractional divisor and a fixed
    // 16x oversample, so what it can reach is set by MCK: 84 MHz gives CD 46 = 114130 baud
    // (-0.93%), while NONE of the three degrade rates reaches 115200 at all: 12 MHz gives
    // CD 7 = 107143 (-7.0%), 6 MHz CD 3 = 125000 (+8.5%) and 4 MHz CD 2 = 125000 (+8.5%),
    // every one past what 8N1 framing tolerates. Deriving it is still what keeps the 84 MHz
    // path correct if MCK ever moves.
    uint32_t uart_brgr_cd(uint32_t mck, uint32_t baud)
    {
        uint32_t const div = 16u * baud;
        uint32_t const cd = (mck + div / 2u) / div;
        if (cd == 0u)
        {
            return 1u;
        }
        return cd;
    }

    // --- TC0 channel 0: the monotonic time base (SAM3X datasheet sec.37) --------
    // arch_clock_now is a REQUIRED chip contract: the armv7m layer ships no clock
    // fallback. The obvious source, the DWT cycle counter, sits in the core debug
    // power domain and intermittently returns aliased garbage on parts in this fleet;
    // the software 32->64 wrap-extension turns one bad read into a phantom 2^32 jump
    // that strands every timed wait. A TC channel is a plain 32-bit peripheral
    // counter: free-run TC0 ch0 in capture mode (WAVE=0, CPCTRG=0 so RC never resets
    // it) off TIMER_CLOCK1 = MCK/2, and use it as arch_clock_now. TC0 ch0 does not
    // collide with the one-shot tickless timer (SysTick, core-generic) nor any driver
    // (none on this port). arch_trace_now stays on raw DWT_CYCCNT.
    constexpr uintptr_t TC0_BASE = 0x40080000;
    constexpr uintptr_t TC0_CCR0 = TC0_BASE + 0x00; // channel control
    constexpr uintptr_t TC0_CMR0 = TC0_BASE + 0x04; // channel mode
    constexpr uintptr_t TC0_CV0 = TC0_BASE + 0x10;  // counter value (read-only)
    constexpr uintptr_t TC0_SR0 = TC0_BASE + 0x20;  // status (read clears flags)
    constexpr uintptr_t TC0_IER0 = TC0_BASE + 0x24; // interrupt enable (write-1-set)
    constexpr uint32_t TC_CMR_TCCLKS_MCK2 = 0x0u << 0; // TIMER_CLOCK1 = MCK/2
    constexpr uint32_t TC_CCR_CLKEN = 1u << 0;
    constexpr uint32_t TC_CCR_SWTRG = 1u << 2;
    constexpr uint32_t TC_SR_COVFS = 1u << 0; // counter overflow status
    constexpr uint32_t PID_TC0 = 1u << 27;    // TC0 channel 0 = peripheral ID 27
    constexpr int TC0_IRQ = 27;               // NVIC line == peripheral ID 27

    // Software 64-bit extension of the 32-bit TC_CV0. Reads are RELIABLE (unlike
    // DWT): the counter wraps every 2^32/42e6 ~= 102 s. The wrap is folded either
    // by a thread read or, when the system is idle with the tickless timer
    // disarmed, by the TC0 overflow (COVFS) ISR below, exactly once: whoever
    // reads first advances g_clk_last, so the other sees no backward step. Without
    // that ISR a wrap across a fully-quiescent >102 s idle would be lost (a slow
    // DWT-style leap). The two words are ONE value: the IrqLock in tc_ticks is what
    // keeps them coherent against the COVFS ISR, not the atomicity of either word.
    uint32_t g_clk_high = 0;
    uint32_t g_clk_last = 0;

    // arch_clock_now epoch anchor (B2, shared: kickos/arch/clk_anchor.h). Sole writer
    // is init() in arch_init; this chip never retunes at runtime. A retune added later
    // must call reprice() at the rate edge; the read must stay pure.
    kickos::arch_clk_anchor g_clk;

    void tc_clock_init()
    {
        // Boot-order: nothing before arch_init may read the clock. A static ctor
        // (__init_array) calling ktime_now()/arch_clock_now() BusFaults here on
        // the ungated TC access.
        // WFI-clocking constraint: TC0 keeps counting in WFI only in Sleep mode
        // (PMC_FSMR.LPM=0, the default). If Wait mode is ever selected MCK stops,
        // freezing TC0 AND SysTick: the whole time base halts, not just this clock.
        r32(PMC_PCER0) = PID_TC0;                 // clock TC0 channel 0
        r32(TC0_CMR0) = TC_CMR_TCCLKS_MCK2;       // MCK/2, capture, RC does not reset
        r32(TC0_CCR0) = TC_CCR_CLKEN | TC_CCR_SWTRG; // enable + start counting
        uint32_t drop = r32(TC0_SR0);             // read-to-clear any pending status
        (void)drop;                               // ((void)r32) would elide the access
        r32(TC0_IER0) = TC_SR_COVFS;              // wrap observer for the idle case
        // No arch_irq_clear_pending: a pend latched here (latch-and-coalesce) redelivers
        // one benign kickos_isr_timer tick on enable, which the tickless handler tolerates.
        arch_irq_unmask(TC0_IRQ);                 // NVIC enable in the maskable band
    }

    // Wrap-catch must be atomic against a concurrent reader (thread + ISR), so the
    // extend runs under the crit section.
    uint64_t tc_ticks()
    {
        arch_irq_state_t s = arch_irq_save();
        uint32_t cur = r32(TC0_CV0);
        if (cur < g_clk_last)
        {
            ++g_clk_high;
        }
        g_clk_last = cur;
        uint64_t hi = g_clk_high;
        arch_irq_restore(s);
        return (hi << 32) | cur;
    }

    void uart_init()
    {
        r32(PMC_PCER0) = PID_UART | PID_PIOA; // clock the UART + its port
        r32(PIOA_PDR) = PA8_PA9;              // PA8/PA9 -> peripheral A (ABSR=0 at reset)
        r32(UART_CR) = CR_RSTRX_RSTTX;
        r32(UART_MR) = MR_NO_PARITY;
        r32(UART_BRGR) = uart_brgr_cd(SystemCoreClock, CONSOLE_BAUD);
        r32(UART_IDR) = 0xFFFFFFFFu; // all UART interrupt sources off; the ring arms TXRDY
        r32(UART_CR) = CR_RXEN_TXEN;
    }

    // --- Buffered console TX backend (console_tx.h). The ring drains via the UART
    // TXRDY interrupt, level-triggered: writing IER.TXRDY while SR.TXRDY=1 (THR
    // empty) raises it immediately. IER/IDR are write-1-to-set/clear (no RMW).
    // slot_free/push touch one data register. ---
    int sam_tx_slot_free(void) { return (r32(UART_SR) & SR_TXRDY) != 0; }
    void sam_tx_push(uint8_t b) { r32(UART_THR) = b; }
    void sam_tx_irq_enable(void) { r32(UART_IER) = IER_TXRDY; }
    void sam_tx_irq_disable(void) { r32(UART_IDR) = IER_TXRDY; }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const sam_console_backend = {
        sam_tx_slot_free, sam_tx_push, sam_tx_irq_enable, sam_tx_irq_disable};

    // NVIC: the dedicated UART is peripheral ID 8, and on the SAM3X the NVIC line
    // equals the peripheral ID -> line 8 (matches PID_UART = 1u << 8 above).
    constexpr int UART_IRQ = 8;
}

extern "C"
{

void arch_init(void)
{
    clock_init(); // crystal + PLLA -> 84 MHz (watchdog already disabled in Reset_Handler)
    tc_clock_init(); // monotonic time base: the required arch_clock_now source
    // Anchor the clock ONCE, from the FINAL rate: TC0 ch0 runs on TIMER_CLOCK1 = MCK/2
    // and MCK == SystemCoreClock, so the ticks advance at half the core clock.
    g_clk.init(SystemCoreClock / 2u);
    uart_init();
    kickos_armv7m_init();
}

// Monotonic clock: free-running TC0 ch0 ticks -> ns, the required per-chip
// arch_clock_now. Pure epoch read: the anchor holds the rate, so no divide and no rate
// derivation happens here.
uint64_t arch_clock_now(void)
{
    return g_clk.ns_from(tc_ticks());
}

// TC0 ch0 overflow (COVFS) ISR, vectored at NVIC 27 in startup.S. Observes the
// 102 s wrap while the tickless timer is disarmed and no thread reads the clock;
// tc_ticks folds it into g_clk_high (idempotent vs a concurrent thread read).
// Runs in the maskable band, so an IrqLock defers it harmlessly.
void kickos_tc0_clock_isr(void)
{
    uint32_t drop = r32(TC0_SR0); // read-to-clear acks COVFS ((void)r32 would elide)
    (void)drop;
    tc_ticks();
}

void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n); // buffered; the routing guard (console.cc) keeps this thread-only
}

void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((r32(UART_SR) & SR_TXRDY) == 0)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(UART_THR) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = UART_IRQ;
    return &sam_console_backend;
}

// Kernel diagnostic LED: "L" LED = PB27 via PIO controller B, active-high.
void arch_diag_led_init(void)
{
    constexpr uintptr_t PIOB_PER = 0x400E1000 + 0x00;
    constexpr uintptr_t PIOB_OER = 0x400E1000 + 0x10;
    r32(PMC_PCER0) = 1u << 12; // clock PIOB (peripheral ID 12)
    r32(PIOB_PER) = 1u << 27;  // pin controlled by the PIO
    r32(PIOB_OER) = 1u << 27;  // output enabled
}

void arch_diag_led_set(int on)
{
    constexpr uintptr_t PIOB_SODR = 0x400E1000 + 0x30;
    constexpr uintptr_t PIOB_CODR = 0x400E1000 + 0x34;
    if (on)
    {
        r32(PIOB_SODR) = 1u << 27;
    }
    else
    {
        r32(PIOB_CODR) = 1u << 27;
    }
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET). func selects GPIO out/in or
// peripheral A/B (see the constant block). Validate range + func + kernel-owned
// BEFORE gating a clock or touching a register (a gate-then-fail path would leak
// an enabled clock).
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port > PINMUX_PORT_MAX or pin > 31u)
    {
        return -KOS_EINVAL;
    }
    if (func != PINMUX_FUNC_GPIO_OUT and func != PINMUX_FUNC_GPIO_IN and
        func != PINMUX_FUNC_PERIPH_A and func != PINMUX_FUNC_PERIPH_B)
    {
        return -KOS_EINVAL;
    }
    if (sam_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    r32(PMC_PCER0) = 1u << (PMC_PID_PIO_SHIFT + port); // clock this PIO (write-1-to-set)
    uintptr_t const base = PIOA_BASE + port * PIO_STRIDE;
    uint32_t const mask = 1u << pin;
    if (func == PINMUX_FUNC_GPIO_OUT)
    {
        r32(base + PIO_PER_OFF) = mask;
        r32(base + PIO_OER_OFF) = mask;
    }
    else if (func == PINMUX_FUNC_GPIO_IN)
    {
        r32(base + PIO_PER_OFF) = mask;
        r32(base + PIO_ODR_OFF) = mask;
    }
    else
    {
        uint32_t absr = r32(base + PIO_ABSR_OFF);
        if (func == PINMUX_FUNC_PERIPH_B)
        {
            absr |= mask;
        }
        else
        {
            absr &= ~mask;
        }
        r32(base + PIO_ABSR_OFF) = absr; // ABSR before PDR: PDR hands the pin to the selected peripheral
        r32(base + PIO_PDR_OFF) = mask;
    }
    return 0;
}

void Reset_Handler(void)
{
    // FIRST: the watchdog is enabled at reset and WDT_MR is write-once, so disable
    // it before anything else can burn the (~16 s) budget or the write.
    r32(WDT_MR) = WDT_MR_WDDIS;
    // Flash (hence the vector table) lives at 0x0008_0000; point VTOR there (the
    // reset SP/PC were fetched via the 0x0 boot alias, which mirrors it).
    r32(SCB_VTOR) = FLASH_BASE;

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
