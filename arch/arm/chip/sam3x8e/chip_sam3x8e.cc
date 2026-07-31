// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Atmel/Microchip AT91SAM3X8E (Arduino Due, Cortex-M3) chip backend. Registers
// clean-room from the SAM3X/SAM3A datasheet (Atmel-11057); hand-rolled, no ASF.
//
// M1 scope: privilege + SVC, no MPU. clock_init() brings the part up on the
// 12 MHz crystal + PLLA to MCK = 84 MHz (SAM3X max); the core boots on the
// imprecise 4 MHz fast RC, at which 115200 is unreachable. Two SAM3X specifics
// that bite: (1) the WATCHDOG runs at reset and WDT_MR is WRITE-ONCE -- it must
// be disabled first thing or the part resets itself; (2) flash is at 0x0008_0000
// (aliased to 0x0 at boot), so the reset path points VTOR at the real table.
// Peripheral clocks are individually gated in the PMC. Console = the dedicated
// UART on PA8/PA9 at a true 115200 once the crystal/PLLA clock is up.
//
// Flashes with bossac over the Due programming port; apps/blink toggles the
// onboard "L" LED (PB27) for a no-UART smoke test.
// Validation status of this port: see docs/reference/boards.md.

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

extern "C"
{
    void kickos_armv7m_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    uint32_t SystemCoreClock = 4000000u; // fast RC at reset; clock_init() raises it to 84 MHz
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
    // covers up to 90 MHz at VDDCORE 1.8V -- required for 84 MHz. Set BEFORE the
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
    // detection -- so MOSCXTST MUST cover the crystal's worst-case warm-up or the
    // status lies and PLLA locks on a still-settling MAINCK (the intermittent-boot
    // race). SLCK is the on-chip slow RC, spec'd 22..42 kHz, so size the count at
    // the FAST end. Time = MOSCXTST * 8 / SLCK. 0x80 = 128 -> 24.4 ms @42 kHz,
    // 31.2 ms @32 kHz, 46.5 ms @22 kHz -- comfortably past the ~15 ms crystal spec.
    // GUESS pending Due silicon: confirm the crystal's startup spec and, if a faster
    // boot matters, trim toward the ~15 ms figure (0x50 gives ~15.2 ms @42 kHz, no
    // margin). Larger = safer but slower boot.
    constexpr uint32_t MOR_KEY = 0x37u << 16;
    constexpr uint32_t MOR_MOSCXTEN = 1u << 0;
    constexpr uint32_t MOR_MOSCRCEN = 1u << 3;
    constexpr uint32_t MOR_MOSCXTST = 0x80u << 8;
    constexpr uint32_t MOR_MOSCSEL = 1u << 24;
    constexpr uint32_t MOR_CRYSTAL = MOR_KEY | MOR_MOSCXTST | MOR_MOSCRCEN | MOR_MOSCXTEN;

    // CKGR_PLLAR (sec.28): PLLA = MAINCK * (MULA+1) / DIVA. ONE (bit 29) reads 1;
    // MULA (26:16) = 13 -> x14; DIVA (7:0) = 1; PLLCOUNT (13:8) = LOCK delay in SLCK.
    // 12 MHz * 14 / 1 = 168 MHz.
    constexpr uint32_t PLLAR_ONE = 1u << 29;
    constexpr uint32_t PLLAR_MULA = 13u << 16;
    constexpr uint32_t PLLAR_COUNT = 0x3Fu << 8;
    constexpr uint32_t PLLAR_DIVA = 1u << 0;

    // PMC_MCKR (sec.28): CSS (1:0) source, PRES (6:4) prescaler. PLLA/2 = 84 MHz.
    constexpr uint32_t MCKR_CSS_MAIN = 1u << 0;
    constexpr uint32_t MCKR_CSS_PLLA = 2u << 0;
    constexpr uint32_t MCKR_PRES_DIV2 = 1u << 4;

    // PMC_SR (sec.28) poll bits.
    constexpr uint32_t SR_MOSCXTS = 1u << 0;   // crystal oscillator stable
    constexpr uint32_t SR_LOCKA = 1u << 1;     // PLLA locked
    constexpr uint32_t SR_MCKRDY = 1u << 3;    // master clock ready
    constexpr uint32_t SR_MOSCSELS = 1u << 16; // main oscillator selection done

    // Bounded poll; true iff the bit set before the bound expired. The bound is a
    // raw spin count on the reset 4 MHz RC: ~1M iterations is hundreds of ms, well
    // past the MOSCXTST window (tens of ms) and every SLCK-counted status delay,
    // so a good crystal always returns true. A false return means the source never
    // came up -- the caller MUST NOT proceed to select it (that is the boot race).
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

    void clock_init()
    {
        // 1. Flash wait states first, both banks (sec.18 / sec.45), before raising
        //    MCK. Over-provisioning is safe: FWS=4 also covers the RC/main fallbacks.
        r32(EEFC0_FMR) = FMR_FWS_4;
        r32(EEFC1_FMR) = FMR_FWS_4;

        // 2. Start the 12 MHz crystal (RC stays MAINCK meanwhile). If MOSCXTS never
        //    asserts there is no usable crystal: stay on the 4 MHz fast RC so the core
        //    and diag LED still run. The UART divisor is computed for 84 MHz MCK, so
        //    the console is unusable on this path -- degraded, not dead-locked.
        r32(CKGR_MOR) = MOR_CRYSTAL;
        if (not pmc_wait(SR_MOSCXTS))
        {
            SystemCoreClock = 4000000u;
            return;
        }

        // 3. Select the crystal as MAINCK, then run MCK off it before touching PLLA.
        r32(CKGR_MOR) = MOR_CRYSTAL | MOR_MOSCSEL;
        if (not pmc_wait(SR_MOSCSELS))
        {
            SystemCoreClock = 4000000u;
            return;
        }
        r32(PMC_MCKR) = MCKR_CSS_MAIN;
        if (not pmc_wait(SR_MCKRDY))
        {
            SystemCoreClock = 4000000u;
            return;
        }

        // 4. PLLA = 12 MHz * 14 / 1 = 168 MHz (sec.28 CKGR_PLLAR). If it never locks,
        //    MCK is already stable on the 12 MHz crystal -- stay there (best-effort;
        //    console still off since BRGR targets 84 MHz).
        r32(CKGR_PLLAR) = PLLAR_ONE | PLLAR_MULA | PLLAR_COUNT | PLLAR_DIVA;
        if (not pmc_wait(SR_LOCKA))
        {
            SystemCoreClock = 12000000u;
            return;
        }

        // 5. Switch MCK to PLLA/2 = 84 MHz. sec.28 mandates, for a PLL source: set
        //    PRES, wait MCKRDY, then set CSS, wait MCKRDY (two writes, not one).
        r32(PMC_MCKR) = MCKR_PRES_DIV2 | MCKR_CSS_MAIN;
        if (not pmc_wait(SR_MCKRDY))
        {
            SystemCoreClock = 12000000u;
            return;
        }
        r32(PMC_MCKR) = MCKR_PRES_DIV2 | MCKR_CSS_PLLA;
        if (not pmc_wait(SR_MCKRDY))
        {
            SystemCoreClock = 12000000u;
            return;
        }

        SystemCoreClock = 84000000u;
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
    // CD = MCK/(16*baud) = 84e6/(16*115200) = 45.57 -> 46; actual 84e6/(16*46) =
    // 114130 baud (-0.93%, well inside the 5% limit in sec.34).
    constexpr uint32_t BRGR_115200 = 46;

    // --- TC0 channel 0: the monotonic time base (SAM3X datasheet sec.37) --------
    // The v7-M default clock is the DWT cycle counter (core debug power domain),
    // which intermittently returns aliased garbage on parts in this fleet; the
    // software 32->64 wrap-extension turns one bad read into a phantom 2^32 jump
    // that strands every timed wait. A TC channel is a plain 32-bit peripheral
    // counter (not the debug domain): free-run TC0 ch0 in capture mode (WAVE=0,
    // CPCTRG=0 so RC never resets it) off TIMER_CLOCK1 = MCK/2, and use it as
    // arch_clock_now. TC0 ch0 does not collide with the one-shot tickless timer
    // (SysTick, core-generic) nor any driver (none on this port). ONLY the
    // monotonic clock moves off DWT; arch_trace_now stays on raw DWT_CYCCNT.
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
    // disarmed, by the TC0 overflow (COVFS) ISR below -- exactly once (whoever
    // reads first advances g_clk_last, so the other sees no backward step). Without
    // that ISR a wrap across a fully-quiescent >102 s idle would be lost (a slow
    // DWT-style leap).
    volatile uint32_t g_clk_high = 0;
    volatile uint32_t g_clk_last = 0;

    // arch_clock_now epoch anchor (B2, shared: kickos/arch/clk_anchor.h). Sole writer
    // is init() in arch_init; this chip never retunes at runtime. A retune added later
    // must call reprice() at the rate edge; the read must stay pure.
    kickos::arch_clk_anchor g_clk;

    void tc_clock_init()
    {
        // Boot-order: nothing before arch_init may read the clock. A static ctor
        // (__init_array) calling ktime_now()/arch_clock_now() BusFaults here on the
        // ungated TC access (it was a harmless DWT read before this override).
        // WFI-clocking constraint: TC0 keeps counting in WFI only in Sleep mode
        // (PMC_FSMR.LPM=0, the default). If Wait mode is ever selected MCK stops,
        // freezing TC0 AND SysTick -- the whole time base halts, not just this clock.
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
            g_clk_high++;
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
        r32(UART_BRGR) = BRGR_115200;
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
    tc_clock_init(); // monotonic time base (replaces the unreliable DWT clock)
    // Anchor the clock ONCE, from the FINAL rate: TC0 ch0 runs on TIMER_CLOCK1 = MCK/2
    // and MCK == SystemCoreClock, so the ticks advance at half the core clock.
    g_clk.init(SystemCoreClock / 2u);
    uart_init();
    kickos_armv7m_init();
}

// Monotonic clock: free-running TC0 ch0 ticks -> ns, the required per-chip source (the
// DWT-backed arch_clock_now (unreliable on this silicon). Pure epoch read: the anchor
// holds the rate, so no divide and no rate derivation happens here.
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
    // FIRST: the watchdog is enabled at reset and WDT_MR is write-once -- disable
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
