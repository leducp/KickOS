// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F103C8 ("Blue Pill", Cortex-M3) chip backend. Registers are clean-room
// from RM0008; hand-rolled, no vendor HAL/CMSIS, consistent with the arch layer.
//
// M1 scope: privilege + SVC, no hardware MPU. Clocking: the Blue Pill carries an
// 8 MHz HSE crystal, so clock_init() runs the PLL (HSE x9) to 72 MHz -- the F103
// max -- for an accurate, full-speed SYSCLK instead of the imprecise HSI RC.
// SYSCLK = HCLK = PCLK2 = 72 MHz, PCLK1 = 36 MHz (its max). Console = USART1 on
// PA9(TX)/PA10(RX), polled TX, on APB2 (72 MHz). F103 uses the older CRL/CRH GPIO
// model (not MODER/AFR) and has no FPU. No watchdog runs at reset, so the reset
// path is just C-runtime. Every RCC/HSE/PLL poll is bounded: a missing or dead
// crystal degrades to the reset HSI clock rather than hanging the boot.
//
// NOT run in this environment (no F103 model here); verified by build + image
// inspection. Flash (ST-LINK/openocd) to confirm; apps/blink toggles the onboard
// LED (PC13, active-low) for a no-UART smoke test.

#include <kickos/arch/arch.h>
#include <kickos/arch/clk_q32.h> // shared Q32 tickless-clock reciprocal + multiply
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

    uint32_t SystemCoreClock = 8000000u; // reset HSI; clock_init() lifts to 72 MHz on PLL
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // RCC (RM0008 sec.7).
    constexpr uintptr_t RCC_BASE = 0x40021000;
    constexpr uintptr_t RCC_CR = RCC_BASE + 0x00;   // Clock control (RM0008 sec.7.3.1, p.99)
    constexpr uintptr_t RCC_CFGR = RCC_BASE + 0x04; // Clock configuration (RM0008 sec.7.3.2, p.101)
    constexpr uintptr_t RCC_APB2ENR = RCC_BASE + 0x18;

    // RCC_CR bits (RM0008 sec.7.3.1, p.99-100).
    constexpr uint32_t CR_HSEON = 1u << 16;
    constexpr uint32_t CR_HSERDY = 1u << 17;
    constexpr uint32_t CR_PLLON = 1u << 24;
    constexpr uint32_t CR_PLLRDY = 1u << 25;

    // RCC_CFGR bits (RM0008 sec.7.3.2, p.101-103).
    constexpr uint32_t CFGR_SW_MASK = 0x3u << 0;
    constexpr uint32_t CFGR_SW_PLL = 0x2u << 0;    // SW=10: PLL as system clock
    constexpr uint32_t CFGR_SWS_MASK = 0x3u << 2;
    constexpr uint32_t CFGR_SWS_PLL = 0x2u << 2;   // SWS=10: PLL used as system clock
    constexpr uint32_t CFGR_HPRE_DIV1 = 0x0u << 4;   // AHB  = SYSCLK/1  = 72 MHz
    constexpr uint32_t CFGR_PPRE1_DIV2 = 0x4u << 8;  // APB1 = HCLK/2    = 36 MHz (36 MHz max)
    constexpr uint32_t CFGR_PPRE2_DIV1 = 0x0u << 11; // APB2 = HCLK/1    = 72 MHz (USART1 clock)
    constexpr uint32_t CFGR_PLLSRC_HSE = 1u << 16;   // PLLSRC=1: HSE feeds the PLL
    constexpr uint32_t CFGR_PLLXTPRE_DIV1 = 0u << 17; // HSE not divided before PLL
    constexpr uint32_t CFGR_PLLMUL9 = 0x7u << 18;    // PLLMUL=0111: input x9 -> 8*9 = 72 MHz

    // FLASH interface (RM0008 sec.3.3.3, p.58-59).
    constexpr uintptr_t FLASH_ACR = 0x40022000;      // Flash access control (RM0008 sec.3.3.3, p.58)
    constexpr uint32_t ACR_LATENCY_2WS = 0x2u << 0;  // LATENCY=010: two wait states (48<SYSCLK<=72 MHz)
    constexpr uint32_t ACR_LATENCY_MASK = 0x7u << 0;
    constexpr uint32_t ACR_PRFTBE = 1u << 4;         // prefetch buffer enable

    // A missing crystal must not hang the boot; every ready-flag poll is bounded.
    constexpr uint32_t CLOCK_POLL_LIMIT = 0x10000u;

    constexpr uint32_t APB2ENR_AFIOEN = 1u << 0;
    constexpr uint32_t APB2ENR_IOPAEN = 1u << 2;
    constexpr uint32_t APB2ENR_USART1EN = 1u << 14;

    // GPIOA (sec.9), CRL/CRH model. USART1 TX=PA9, RX=PA10 live in CRH (pins 8-15).
    constexpr uintptr_t GPIOA_BASE = 0x40010800;
    constexpr uintptr_t GPIOA_CRH = GPIOA_BASE + 0x04;
    // PA9  = AF push-pull, 50 MHz : CNF=10 MODE=11 -> nibble 0xB, bits [7:4]
    // PA10 = input floating       : CNF=01 MODE=00 -> nibble 0x4, bits [11:8]
    constexpr uint32_t CRH_PA9 = 0xBu << 4;
    constexpr uint32_t CRH_PA10 = 0x4u << 8;
    constexpr uint32_t CRH_PA9_PA10_MASK = (0xFu << 4) | (0xFu << 8);

    // USART1 (sec.27), classic SR/DR. On APB2 (PCLK2 = 72 MHz after clock_init).
    constexpr uintptr_t USART1_BASE = 0x40013800;
    constexpr uintptr_t USART1_SR = USART1_BASE + 0x00;
    constexpr uintptr_t USART1_DR = USART1_BASE + 0x04;
    constexpr uintptr_t USART1_BRR = USART1_BASE + 0x08;
    constexpr uintptr_t USART1_CR1 = USART1_BASE + 0x0C;
    constexpr uint32_t SR_TXE = 1u << 7;
    constexpr uint32_t CR1_UE = 1u << 13;
    constexpr uint32_t CR1_TE = 1u << 3;
    constexpr uint32_t CR1_RE = 1u << 2;
    constexpr uint32_t CR1_TXEIE = 1u << 7; // TX-data-register-empty interrupt enable
    // BRR at PCLK2 = 72 MHz, 115200 baud, oversampling 16 (RM0008 sec.27.3.4, sec.27.6.3):
    //   USARTDIV = 72e6 / (16 * 115200) = 39.0625
    //   mantissa = 39 (0x27), fraction = round(0.0625 * 16) = 1  -> exact, 0% error
    //   BRR = (39 << 4) | 1 = 0x271
    constexpr uint32_t BRR_115200 = (39u << 4) | 1u; // 0x271

    // Bounded ready-flag poll: returns true once (reg & mask) == mask, false if
    // the bit never sets within the budget (dead/absent HSE crystal).
    bool poll_set(uintptr_t reg, uint32_t mask)
    {
        for (uint32_t i = 0; i < CLOCK_POLL_LIMIT; i++)
        {
            if ((r32(reg) & mask) == mask)
            {
                return true;
            }
        }
        return false;
    }

    // Bring SYSCLK to 72 MHz on HSE(8 MHz) x PLL9. Order matters: flash wait states
    // must be set BEFORE raising the clock, else instruction fetches at 72 MHz with
    // 0 WS would fail. If HSE or the PLL never locks we leave the reset HSI clock
    // (8 MHz) selected and return, so a boardswapped/crystalless part still boots.
    void clock_init()
    {
        // 1. Flash: 2 wait states + prefetch, ahead of the switch (RM0008 sec.3.3.3, p.59).
        uint32_t acr = r32(FLASH_ACR);
        acr &= ~ACR_LATENCY_MASK;
        acr |= ACR_LATENCY_2WS | ACR_PRFTBE;
        r32(FLASH_ACR) = acr;

        // 2. Enable HSE and wait for it to stabilize (RM0008 sec.7.3.1, HSEON/HSERDY).
        r32(RCC_CR) |= CR_HSEON;
        if (!poll_set(RCC_CR, CR_HSERDY))
        {
            return; // no crystal -> stay on HSI
        }

        // 3. PLL source/multiplier + bus prescalers. PLLMUL/PLLSRC are writable only
        //    while the PLL is off, which it is at reset (RM0008 sec.7.3.2, p.101-103).
        uint32_t cfgr = r32(RCC_CFGR);
        cfgr &= ~(0xFu << 4);    // clear HPRE  [7:4]
        cfgr &= ~(0x7u << 8);    // clear PPRE1 [10:8]
        cfgr &= ~(0x7u << 11);   // clear PPRE2 [13:11]
        cfgr &= ~(0xFu << 18);   // clear PLLMUL [21:18]
        cfgr &= ~(CFGR_PLLSRC_HSE | (1u << 17));
        cfgr |= CFGR_HPRE_DIV1 | CFGR_PPRE1_DIV2 | CFGR_PPRE2_DIV1;
        cfgr |= CFGR_PLLSRC_HSE | CFGR_PLLXTPRE_DIV1 | CFGR_PLLMUL9;
        r32(RCC_CFGR) = cfgr;

        // 4. Enable the PLL and wait for lock (RM0008 sec.7.3.1, PLLON/PLLRDY).
        r32(RCC_CR) |= CR_PLLON;
        if (!poll_set(RCC_CR, CR_PLLRDY))
        {
            return; // PLL failed -> stay on HSI
        }

        // Switch SYSCLK to the PLL and confirm via SWS (RM0008 sec.7.3.2, SW/SWS).
        uint32_t sw = r32(RCC_CFGR);
        sw &= ~CFGR_SW_MASK;
        sw |= CFGR_SW_PLL;
        r32(RCC_CFGR) = sw;
        for (uint32_t i = 0; i < CLOCK_POLL_LIMIT; i++)
        {
            if ((r32(RCC_CFGR) & CFGR_SWS_MASK) == CFGR_SWS_PLL)
            {
                SystemCoreClock = 72000000u;
                return;
            }
        }
    }

    // --- TIM2->TIM3 chain: the monotonic time base (RM0008 sec.15) --------------
    // The v7-M default clock is the DWT cycle counter (core debug power domain),
    // which intermittently returns aliased garbage on parts in this fleet; the
    // software 32->64 wrap-extension turns one bad read into a phantom 2^32 jump
    // that strands every timed wait. The F1 has no 32-bit timer -- all GP timers
    // are 16-bit -- so a single free-runner would wrap every ~0.9 ms and lose
    // whole wraps between clock reads (the same missed-wrap failure class as DWT).
    // Instead chain two 16-bit timers into a 32-bit counter: TIM2 (master) counts
    // at the timer kernel clock and emits TRGO on each overflow; TIM3 (slave, ext
    // clock mode 1 off ITR1=TIM2) counts those overflows -> {TIM3:TIM2} is one
    // 32-bit counter that wraps every ~59 s. Neither timer collides with the
    // one-shot tickless timer (SysTick, core-generic) nor any driver (none on this
    // port). ONLY the monotonic clock moves off DWT; arch_trace_now stays on DWT.
    constexpr uintptr_t RCC_APB1ENR = RCC_BASE + 0x1C;
    constexpr uint32_t APB1ENR_TIM2EN = 1u << 0;
    constexpr uint32_t APB1ENR_TIM3EN = 1u << 1;

    constexpr uintptr_t TIM2_BASE = 0x40000000;
    constexpr uintptr_t TIM3_BASE = 0x40000400;
    constexpr uintptr_t TIM2_CR1 = TIM2_BASE + 0x00;
    constexpr uintptr_t TIM2_CR2 = TIM2_BASE + 0x04;
    constexpr uintptr_t TIM2_EGR = TIM2_BASE + 0x14;
    constexpr uintptr_t TIM2_CNT = TIM2_BASE + 0x24;
    constexpr uintptr_t TIM2_PSC = TIM2_BASE + 0x28;
    constexpr uintptr_t TIM2_ARR = TIM2_BASE + 0x2C;
    constexpr uintptr_t TIM3_CR1 = TIM3_BASE + 0x00;
    constexpr uintptr_t TIM3_SMCR = TIM3_BASE + 0x08;
    constexpr uintptr_t TIM3_DIER = TIM3_BASE + 0x0C;
    constexpr uintptr_t TIM3_SR = TIM3_BASE + 0x10;
    constexpr uintptr_t TIM3_EGR = TIM3_BASE + 0x14;
    constexpr uintptr_t TIM3_CNT = TIM3_BASE + 0x24;
    constexpr uintptr_t TIM3_PSC = TIM3_BASE + 0x28;
    constexpr uintptr_t TIM3_ARR = TIM3_BASE + 0x2C;
    constexpr uint32_t TIM_CR1_CEN = 1u << 0;
    constexpr uint32_t TIM_EGR_UG = 1u << 0;
    constexpr uint32_t TIM_DIER_UIE = 1u << 0; // update (overflow) interrupt enable
    constexpr uint32_t TIM_SR_UIF = 1u << 0;   // update (overflow) flag, rc_w0
    constexpr uint32_t TIM2_CR2_MMS_UPDATE = 0x2u << 4; // MMS=010: TRGO on update
    constexpr uint32_t TIM3_SMCR_TS_ITR1 = 0x1u << 4;   // TS=001: trigger = ITR1 = TIM2
    constexpr uint32_t TIM3_SMCR_SMS_EXT1 = 0x7u << 0;  // SMS=111: ext clock mode 1
    constexpr int TIM3_IRQ = 29;                        // NVIC position 29 = TIM3 (RM0008)

    // Software 64-bit extension of the 32-bit chained counter. Reads are RELIABLE
    // (unlike DWT): the pair wraps every 2^32/72e6 ~= 59 s. The wrap is folded
    // either by a thread read or, when the system is idle with the tickless timer
    // disarmed, by the TIM3 (high-half) overflow ISR below -- exactly once (whoever
    // reads first advances g_clk_last, so the other sees no backward step). Without
    // that ISR a wrap across a fully-quiescent >59 s idle would be lost (a slow
    // DWT-style leap).
    volatile uint32_t g_clk_high = 0;
    volatile uint32_t g_clk_last = 0;

    void timer_clock_init()
    {
        // Boot-order: nothing before arch_init may read the clock. A static ctor
        // (__init_array) calling ktime_now()/arch_clock_now() BusFaults here on the
        // ungated APB1 access (it was a harmless DWT read before this override).
        r32(RCC_APB1ENR) |= APB1ENR_TIM2EN | APB1ENR_TIM3EN;

        // TIM2 master: free-run 16-bit, emit TRGO on each overflow.
        r32(TIM2_CR1) = 0;
        r32(TIM2_PSC) = 0;
        r32(TIM2_ARR) = 0x0000FFFFu;
        r32(TIM2_CR2) = TIM2_CR2_MMS_UPDATE;
        r32(TIM2_EGR) = TIM_EGR_UG;

        // TIM3 slave: clocked by TIM2's TRGO (ITR1), free-run 16-bit. Its overflow
        // (the 32-bit chain wrap) drives the idle wrap observer.
        r32(TIM3_CR1) = 0;
        r32(TIM3_PSC) = 0;
        r32(TIM3_ARR) = 0x0000FFFFu;
        r32(TIM3_SMCR) = TIM3_SMCR_TS_ITR1 | TIM3_SMCR_SMS_EXT1;
        r32(TIM3_EGR) = TIM_EGR_UG;
        r32(TIM3_SR) = ~TIM_SR_UIF;    // drop the UG-induced UIF before arming the IRQ
        r32(TIM3_DIER) = TIM_DIER_UIE; // wrap observer for the disarmed-timer idle case

        // Enable the slave first so no master TRGO edge is missed, then the master.
        r32(TIM3_CR1) = TIM_CR1_CEN;
        r32(TIM2_CR1) = TIM_CR1_CEN;
        // No arch_irq_clear_pending: a pend latched here (latch-and-coalesce) redelivers
        // one benign kickos_isr_timer tick on enable, which the tickless handler tolerates.
        arch_irq_unmask(TIM3_IRQ);     // NVIC enable in the maskable device band
    }

    // Read the chained 32-bit counter torn-read-safe (re-read the TIM3 high half
    // last: a stable high half validates the low half against a straddled TIM2
    // roll-under), then software-extend to 64-bit. The whole read runs under the
    // crit section so the wrap-catch is atomic against a concurrent reader (the
    // timers themselves keep counting regardless of the IRQ mask, hence the retry).
    uint64_t timer_ticks()
    {
        arch_irq_state_t s = arch_irq_save();
        uint32_t hi = r32(TIM3_CNT) & 0xFFFFu;
        uint32_t lo;
        while (true)
        {
            lo = r32(TIM2_CNT) & 0xFFFFu;
            uint32_t hi2 = r32(TIM3_CNT) & 0xFFFFu;
            if (hi2 == hi)
            {
                break;
            }
            hi = hi2;
        }
        uint32_t cur = (hi << 16) | lo;
        // The hi/lo/hi guard still admits a master-wrap(TIM2)->slave-increment(TIM3)
        // SKEW: lo can be post-wrap-low while hi is still pre-increment, yielding a
        // value up to ~65536 BELOW the last read. A magnitude discriminator tells
        // that tear from a genuine 32-bit wrap (gap ~2^32): only a large gap bumps
        // the high half; a small backward tear is clamped so the clock stays
        // monotonic (misreading the tear as a wrap would leap the clock +59.6 s).
        if (cur < g_clk_last)
        {
            if (g_clk_last - cur > 0x80000000u)
            {
                g_clk_high++;       // genuine chain wrap
            }
            else
            {
                cur = g_clk_last;   // torn chained read: clamp, stay monotonic
            }
        }
        g_clk_last = cur;
        uint64_t high = g_clk_high;
        arch_irq_restore(s);
        return (high << 32) | cur;
    }

    void usart1_init()
    {
        r32(RCC_APB2ENR) |= APB2ENR_IOPAEN | APB2ENR_AFIOEN | APB2ENR_USART1EN;

        uint32_t crh = r32(GPIOA_CRH);
        crh &= ~CRH_PA9_PA10_MASK;
        crh |= CRH_PA9 | CRH_PA10;
        r32(GPIOA_CRH) = crh;

        r32(USART1_CR1) = 0;         // disable while configuring
        r32(USART1_BRR) = BRR_115200;
        r32(USART1_CR1) = CR1_UE | CR1_TE | CR1_RE; // TXEIE stays clear; the ring primes it
    }

    // --- Buffered console TX backend (console_tx.h). The ring drains via the
    // USART1 TXE (TX-data-register-empty) interrupt, which is level-triggered:
    // enabling TXEIE while TXE=1 raises it immediately. slot_free/push touch one
    // data register; irq_enable/disable gate TXEIE at the peripheral. ---
    int f1_tx_slot_free(void) { return (r32(USART1_SR) & SR_TXE) != 0; }
    void f1_tx_push(uint8_t b) { r32(USART1_DR) = b; }
    void f1_tx_irq_enable(void) { r32(USART1_CR1) |= CR1_TXEIE; }
    void f1_tx_irq_disable(void) { r32(USART1_CR1) &= ~CR1_TXEIE; }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const f1_console_backend = {
        f1_tx_slot_free, f1_tx_push, f1_tx_irq_enable, f1_tx_irq_disable};

    // NVIC: USART1 global interrupt (RX/TX combined) = line 37 (RM0008 vector
    // table). Only TXEIE is armed, so the drain ISR is the sole source.
    constexpr int USART1_IRQ = 37;
}

extern "C"
{

void arch_init(void)
{
    // Clock first (HSE/PLL -> 72 MHz), then the console derives its BRR from PCLK2.
    clock_init();
    timer_clock_init(); // monotonic time base (replaces the unreliable DWT clock)
    usart1_init();
    kickos_armv7m_init();
}

// Monotonic clock override: free-running TIM2->TIM3 chain ticks -> ns, replacing
// the weak DWT-backed arch_clock_now (unreliable on this silicon). The chained
// counter's LSB increments at TIM2's kernel clock; with HPRE=/1 and PPRE1 in
// {/1,/2} the STM32 APB timer-clock doubler makes that equal HCLK ==
// SystemCoreClock across the PLL and the HSI-fallback states. ns = ticks*1e9/hz
// via a cached reciprocal multiply (the one 64-bit divide runs only at a change).
uint64_t arch_clock_now(void)
{
    uint32_t tim_hz = SystemCoreClock;
    static uint64_t cached_hz = 0;
    static uint64_t mult = 0;
    if (tim_hz != cached_hz)
    {
        if (tim_hz == 0)
        {
            return 0;
        }
        mult = kickos::arch_clk_recip_q32(tim_hz);
        cached_hz = tim_hz;
    }
    uint64_t ticks = timer_ticks();
    return kickos::arch_clk_mul_q32(ticks, mult);
}

// TIM3 (high-half) overflow ISR, vectored at NVIC 29 in startup.S. Observes the
// 59 s chain wrap while the tickless timer is disarmed and no thread reads the
// clock; timer_ticks folds it into g_clk_high (idempotent vs a concurrent thread
// read). Runs in the maskable band, so an IrqLock defers it harmlessly.
void kickos_tim3_clock_isr(void)
{
    r32(TIM3_SR) = ~TIM_SR_UIF; // ack the update flag (rc_w0)
    timer_ticks();
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
        while ((r32(USART1_SR) & SR_TXE) == 0)
        {
            if (++spin > 1000000u)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(USART1_DR) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = USART1_IRQ;
    return &f1_console_backend;
}

// Kernel diagnostic LED: PC13, active-LOW (lit when the pin is driven low).
void arch_diag_led_init(void)
{
    constexpr uintptr_t GPIOC_CRH = 0x40011000 + 0x04;
    r32(RCC_APB2ENR) |= (1u << 4); // IOPCEN (GPIOC)
    uint32_t crh = r32(GPIOC_CRH);
    crh &= ~(0xFu << 20);          // clear PC13 nibble
    crh |= (0x2u << 20);          // general-purpose push-pull, 2 MHz
    r32(GPIOC_CRH) = crh;
}

void arch_diag_led_set(int on)
{
    constexpr uintptr_t GPIOC_BSRR = 0x40011000 + 0x10;
    if (on)
    {
        r32(GPIOC_BSRR) = 1u << (13 + 16); // BR13 -> PC13 low -> LED on
    }
    else
    {
        r32(GPIOC_BSRR) = 1u << 13;        // BS13 -> PC13 high -> LED off
    }
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
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
