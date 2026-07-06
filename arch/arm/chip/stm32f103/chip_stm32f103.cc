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

    // RCC (RM0008 §7).
    constexpr uintptr_t RCC_BASE = 0x40021000;
    constexpr uintptr_t RCC_CR = RCC_BASE + 0x00;   // Clock control (RM0008 §7.3.1, p.99)
    constexpr uintptr_t RCC_CFGR = RCC_BASE + 0x04; // Clock configuration (RM0008 §7.3.2, p.101)
    constexpr uintptr_t RCC_APB2ENR = RCC_BASE + 0x18;

    // RCC_CR bits (RM0008 §7.3.1, p.99-100).
    constexpr uint32_t CR_HSEON = 1u << 16;
    constexpr uint32_t CR_HSERDY = 1u << 17;
    constexpr uint32_t CR_PLLON = 1u << 24;
    constexpr uint32_t CR_PLLRDY = 1u << 25;

    // RCC_CFGR bits (RM0008 §7.3.2, p.101-103).
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

    // FLASH interface (RM0008 §3.3.3, p.58-59).
    constexpr uintptr_t FLASH_ACR = 0x40022000;      // Flash access control (RM0008 §3.3.3, p.58)
    constexpr uint32_t ACR_LATENCY_2WS = 0x2u << 0;  // LATENCY=010: two wait states (48<SYSCLK<=72 MHz)
    constexpr uint32_t ACR_LATENCY_MASK = 0x7u << 0;
    constexpr uint32_t ACR_PRFTBE = 1u << 4;         // prefetch buffer enable

    // A missing crystal must not hang the boot; every ready-flag poll is bounded.
    constexpr uint32_t CLOCK_POLL_LIMIT = 0x10000u;

    constexpr uint32_t APB2ENR_AFIOEN = 1u << 0;
    constexpr uint32_t APB2ENR_IOPAEN = 1u << 2;
    constexpr uint32_t APB2ENR_USART1EN = 1u << 14;

    // GPIOA (§9), CRL/CRH model. USART1 TX=PA9, RX=PA10 live in CRH (pins 8-15).
    constexpr uintptr_t GPIOA_BASE = 0x40010800;
    constexpr uintptr_t GPIOA_CRH = GPIOA_BASE + 0x04;
    // PA9  = AF push-pull, 50 MHz : CNF=10 MODE=11 -> nibble 0xB, bits [7:4]
    // PA10 = input floating       : CNF=01 MODE=00 -> nibble 0x4, bits [11:8]
    constexpr uint32_t CRH_PA9 = 0xBu << 4;
    constexpr uint32_t CRH_PA10 = 0x4u << 8;
    constexpr uint32_t CRH_PA9_PA10_MASK = (0xFu << 4) | (0xFu << 8);

    // USART1 (§27), classic SR/DR. On APB2 (PCLK2 = 72 MHz after clock_init).
    constexpr uintptr_t USART1_BASE = 0x40013800;
    constexpr uintptr_t USART1_SR = USART1_BASE + 0x00;
    constexpr uintptr_t USART1_DR = USART1_BASE + 0x04;
    constexpr uintptr_t USART1_BRR = USART1_BASE + 0x08;
    constexpr uintptr_t USART1_CR1 = USART1_BASE + 0x0C;
    constexpr uint32_t SR_TXE = 1u << 7;
    constexpr uint32_t CR1_UE = 1u << 13;
    constexpr uint32_t CR1_TE = 1u << 3;
    constexpr uint32_t CR1_RE = 1u << 2;
    // BRR at PCLK2 = 72 MHz, 115200 baud, oversampling 16 (RM0008 §27.3.4, §27.6.3):
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
        // 1. Flash: 2 wait states + prefetch, ahead of the switch (RM0008 §3.3.3, p.59).
        uint32_t acr = r32(FLASH_ACR);
        acr &= ~ACR_LATENCY_MASK;
        acr |= ACR_LATENCY_2WS | ACR_PRFTBE;
        r32(FLASH_ACR) = acr;

        // 2. Enable HSE and wait for it to stabilize (RM0008 §7.3.1, HSEON/HSERDY).
        r32(RCC_CR) |= CR_HSEON;
        if (!poll_set(RCC_CR, CR_HSERDY))
        {
            return; // no crystal -> stay on HSI
        }

        // 3. PLL source/multiplier + bus prescalers. PLLMUL/PLLSRC are writable only
        //    while the PLL is off, which it is at reset (RM0008 §7.3.2, p.101-103).
        uint32_t cfgr = r32(RCC_CFGR);
        cfgr &= ~(0xFu << 4);    // clear HPRE  [7:4]
        cfgr &= ~(0x7u << 8);    // clear PPRE1 [10:8]
        cfgr &= ~(0x7u << 11);   // clear PPRE2 [13:11]
        cfgr &= ~(0xFu << 18);   // clear PLLMUL [21:18]
        cfgr &= ~(CFGR_PLLSRC_HSE | (1u << 17));
        cfgr |= CFGR_HPRE_DIV1 | CFGR_PPRE1_DIV2 | CFGR_PPRE2_DIV1;
        cfgr |= CFGR_PLLSRC_HSE | CFGR_PLLXTPRE_DIV1 | CFGR_PLLMUL9;
        r32(RCC_CFGR) = cfgr;

        // 4. Enable the PLL and wait for lock (RM0008 §7.3.1, PLLON/PLLRDY).
        r32(RCC_CR) |= CR_PLLON;
        if (!poll_set(RCC_CR, CR_PLLRDY))
        {
            return; // PLL failed -> stay on HSI
        }

        // Switch SYSCLK to the PLL and confirm via SWS (RM0008 §7.3.2, SW/SWS).
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

    void usart1_init()
    {
        r32(RCC_APB2ENR) |= APB2ENR_IOPAEN | APB2ENR_AFIOEN | APB2ENR_USART1EN;

        uint32_t crh = r32(GPIOA_CRH);
        crh &= ~CRH_PA9_PA10_MASK;
        crh |= CRH_PA9 | CRH_PA10;
        r32(GPIOA_CRH) = crh;

        r32(USART1_CR1) = 0;         // disable while configuring
        r32(USART1_BRR) = BRR_115200;
        r32(USART1_CR1) = CR1_UE | CR1_TE | CR1_RE;
    }
}

extern "C"
{

void arch_init(void)
{
    // Clock first (HSE/PLL -> 72 MHz), then the console derives its BRR from PCLK2.
    clock_init();
    usart1_init();
    kickos_armv7m_init();
}

void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        while ((r32(USART1_SR) & SR_TXE) == 0)
        {
        }
        r32(USART1_DR) = static_cast<uint8_t>(buf[i]);
    }
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

void HardFault_Handler(void)
{
    arch_shutdown(132);
}

}
