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
// Build-only here; flash with bossac (the Due programming port). apps/blink
// toggles the onboard "L" LED (PB27) for a no-UART smoke test.

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>

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
        if (!pmc_wait(SR_MOSCXTS))
        {
            SystemCoreClock = 4000000u;
            return;
        }

        // 3. Select the crystal as MAINCK, then run MCK off it before touching PLLA.
        r32(CKGR_MOR) = MOR_CRYSTAL | MOR_MOSCSEL;
        if (!pmc_wait(SR_MOSCSELS))
        {
            SystemCoreClock = 4000000u;
            return;
        }
        r32(PMC_MCKR) = MCKR_CSS_MAIN;
        if (!pmc_wait(SR_MCKRDY))
        {
            SystemCoreClock = 4000000u;
            return;
        }

        // 4. PLLA = 12 MHz * 14 / 1 = 168 MHz (sec.28 CKGR_PLLAR). If it never locks,
        //    MCK is already stable on the 12 MHz crystal -- stay there (best-effort;
        //    console still off since BRGR targets 84 MHz).
        r32(CKGR_PLLAR) = PLLAR_ONE | PLLAR_MULA | PLLAR_COUNT | PLLAR_DIVA;
        if (!pmc_wait(SR_LOCKA))
        {
            SystemCoreClock = 12000000u;
            return;
        }

        // 5. Switch MCK to PLLA/2 = 84 MHz. sec.28 mandates, for a PLL source: set
        //    PRES, wait MCKRDY, then set CSS, wait MCKRDY (two writes, not one).
        r32(PMC_MCKR) = MCKR_PRES_DIV2 | MCKR_CSS_MAIN;
        if (!pmc_wait(SR_MCKRDY))
        {
            SystemCoreClock = 12000000u;
            return;
        }
        r32(PMC_MCKR) = MCKR_PRES_DIV2 | MCKR_CSS_PLLA;
        if (!pmc_wait(SR_MCKRDY))
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
    // monotonic clock moves off DWT; arch_trace_now stays on raw DWT_CYCCNT. The
    // low-res RTT is avoided; a 32-bit TC at 42 MHz is a clean full-range source.
    constexpr uintptr_t TC0_BASE = 0x40080000;
    constexpr uintptr_t TC0_CCR0 = TC0_BASE + 0x00; // channel control
    constexpr uintptr_t TC0_CMR0 = TC0_BASE + 0x04; // channel mode
    constexpr uintptr_t TC0_CV0 = TC0_BASE + 0x10;  // counter value (read-only)
    constexpr uint32_t TC_CMR_TCCLKS_MCK2 = 0x0u << 0; // TIMER_CLOCK1 = MCK/2
    constexpr uint32_t TC_CCR_CLKEN = 1u << 0;
    constexpr uint32_t TC_CCR_SWTRG = 1u << 2;
    constexpr uint32_t PID_TC0 = 1u << 27; // TC0 channel 0 = peripheral ID 27

    // Software 64-bit extension of the 32-bit TC_CV0. Reads are RELIABLE (unlike
    // DWT): the counter wraps every 2^32/42e6 ~= 102 s and the tickless re-arm
    // reads the clock several times per second (SysTick 24-bit caps a single arm
    // near 0.2 s), so a wrap is always observed before the next one.
    volatile uint32_t g_clk_high = 0;
    volatile uint32_t g_clk_last = 0;

    void tc_clock_init()
    {
        // Boot-order: arch_clock_now MUST NOT run before this (an ungated TC read
        // would fault). arch_init calls it before kickos_armv7m_init.
        r32(PMC_PCER0) = PID_TC0;                 // clock TC0 channel 0
        r32(TC0_CMR0) = TC_CMR_TCCLKS_MCK2;       // MCK/2, capture, RC does not reset
        r32(TC0_CCR0) = TC_CCR_CLKEN | TC_CCR_SWTRG; // enable + start counting
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
    uart_init();
    kickos_armv7m_init();
}

// Monotonic clock override: free-running TC0 ch0 ticks -> ns, replacing the weak
// DWT-backed arch_clock_now (unreliable on this silicon). TC0 ch0 runs on
// TIMER_CLOCK1 = MCK/2, and MCK == SystemCoreClock, so the tick rate is
// SystemCoreClock/2 at the PLL rate and at every clock-fallback state.
// ns = ticks*1e9/hz via a cached reciprocal multiply (the one 64-bit divide runs
// only at a clock change).
uint64_t arch_clock_now(void)
{
    uint32_t tc_hz = SystemCoreClock / 2u; // TIMER_CLOCK1 = MCK/2
    static uint64_t cached_hz = 0;
    static uint64_t mult = 0;
    if (tc_hz != cached_hz)
    {
        if (tc_hz == 0)
        {
            return 0;
        }
        mult = ((1000000000ull << 32) + (tc_hz >> 1)) / tc_hz;
        cached_hz = tc_hz;
    }
    uint64_t ticks = tc_ticks();
    uint64_t a = ticks >> 32, b = ticks & 0xFFFFFFFFull;
    uint64_t c = mult >> 32, d = mult & 0xFFFFFFFFull;
    return ((a * c) << 32) + a * d + b * c + ((b * d) >> 32);
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
            if (++spin > 1000000u)
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

void arch_shutdown(int status)
{
    (void)status;
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
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
