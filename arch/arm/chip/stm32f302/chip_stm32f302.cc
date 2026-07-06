// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F302R8 (Nucleo-F302R8, Cortex-M4F) chip backend. Registers clean-room from
// RM0365; hand-rolled, no vendor HAL. Versus the F411: the F3 puts the GPIO ports
// on AHB at 0x4800_0000 (not 0x4002_0000), the RCC block is at 0x4002_1000, and
// the USART is the NEWER model (ISR/TDR, not SR/DR).
//
// M1 scope: privilege + SVC, no MPU. The Nucleo-F302R8 carries no crystal, so
// clock_init() runs the PLL from HSI/2 (the most portable path -- no HSE, no MCO
// solder-bridge dependency, identical on every board): HSI/2 (4 MHz) x16 = 64 MHz
// SYSCLK. HCLK=64, PCLK2=64, PCLK1=32 (its 36 MHz max). USART2 is on APB1, so its
// BRR is derived from the achieved PCLK1. Console = USART2 on PA2/PA3 (AF7, the
// ST-LINK VCP), polled TX. No watchdog runs at reset. Every HSI/PLL poll is
// bounded: if the PLL never locks the boot degrades to the reset HSI 8 MHz clock
// rather than hanging. Flash to confirm, or watch LD2 (PB13) blink.

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

    uint32_t SystemCoreClock = 8000000u; // reset HSI; clock_init() lifts to 64 MHz on PLL
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // APB1 clock feeding USART2. Reset HSI => PCLK1 = 8 MHz; clock_init sets 32 MHz.
    uint32_t pclk1_hz = 8000000u;

    // RCC (RM0365 §9): F3 layout, base 0x40021000.
    constexpr uintptr_t RCC_BASE = 0x40021000;
    constexpr uintptr_t RCC_CR = RCC_BASE + 0x00;   // Clock control (RM0365 §9.4.1)
    constexpr uintptr_t RCC_CFGR = RCC_BASE + 0x04; // Clock configuration (RM0365 §9.4.2)
    constexpr uintptr_t RCC_AHBENR = RCC_BASE + 0x14;
    constexpr uintptr_t RCC_APB1ENR = RCC_BASE + 0x1C;
    constexpr uint32_t AHBENR_IOPAEN = 1u << 17; // GPIOA (ports are on AHB)
    constexpr uint32_t APB1ENR_USART2EN = 1u << 17;

    // RCC_CR (RM0365 §9.4.1).
    constexpr uint32_t CR_HSION = 1u << 0;
    constexpr uint32_t CR_HSIRDY = 1u << 1;
    constexpr uint32_t CR_PLLON = 1u << 24;
    constexpr uint32_t CR_PLLRDY = 1u << 25;

    // RCC_CFGR (RM0365 §9.4.2). PLLSRC(16)=0 -> HSI/2 feeds the PLL; PLLMUL[21:18]
    // = 1110B (x16). HSI/2 = 4 MHz * 16 = 64 MHz. Buses: AHB /1, APB1 /2 (32 MHz,
    // <= its 36 MHz max), APB2 /1 (64 MHz).
    constexpr uint32_t CFGR_SW_MASK = 0x3u << 0;
    constexpr uint32_t CFGR_SW_PLL = 0x2u << 0;
    constexpr uint32_t CFGR_SWS_MASK = 0x3u << 2;
    constexpr uint32_t CFGR_SWS_PLL = 0x2u << 2;
    constexpr uint32_t CFGR_HPRE_DIV1 = 0x0u << 4;
    constexpr uint32_t CFGR_PPRE1_DIV2 = 0x4u << 8;
    constexpr uint32_t CFGR_PPRE2_DIV1 = 0x0u << 11;
    constexpr uint32_t CFGR_PLLSRC_HSI_DIV2 = 0x0u << 16;
    constexpr uint32_t CFGR_PLLMUL16 = 0xEu << 18;
    constexpr uint32_t CFGR_PLL_FIELDS_MASK =
        (0xFu << 4) | (0x7u << 8) | (0x7u << 11) | (0x1u << 16) | (0xFu << 18);

    // FLASH interface (F3 shares the F0 layout): ACR at 0x40022000. LATENCY[2:0]=2
    // for 48 < SYSCLK <= 72 MHz; PRFTBE(4) prefetch enable.
    constexpr uintptr_t FLASH_ACR = 0x40022000;
    constexpr uint32_t ACR_LATENCY_2WS = 0x2u << 0;
    constexpr uint32_t ACR_LATENCY_MASK = 0x7u << 0;
    constexpr uint32_t ACR_PRFTBE = 1u << 4;

    // A missing/failed PLL must not hang the boot; every ready-flag poll is bounded.
    constexpr uint32_t CLOCK_POLL_LIMIT = 0x100000u;

    // GPIOA on AHB at 0x48000000 (§11). MODER 2b/pin, AFRL 4b/pin.
    constexpr uintptr_t GPIOA_BASE = 0x48000000;
    constexpr uintptr_t GPIOA_MODER = GPIOA_BASE + 0x00;
    constexpr uintptr_t GPIOA_AFRL = GPIOA_BASE + 0x20;

    // USART2 (§29), NEW model. On APB1 (PCLK1 = 32 MHz after clock_init).
    constexpr uintptr_t USART2_BASE = 0x40004400;
    constexpr uintptr_t USART2_CR1 = USART2_BASE + 0x00;
    constexpr uintptr_t USART2_BRR = USART2_BASE + 0x0C;
    constexpr uintptr_t USART2_ISR = USART2_BASE + 0x1C;
    constexpr uintptr_t USART2_TDR = USART2_BASE + 0x28;
    constexpr uint32_t ISR_TXE = 1u << 7;
    constexpr uint32_t CR1_UE = 1u << 0;
    constexpr uint32_t CR1_TE = 1u << 3;
    constexpr uint32_t CR1_RE = 1u << 2;

    // USART2SEL resets to 00 -> PCLK1 clocks USART2. OVER8=0 -> BRR = fck/baud,
    // rounded to nearest. At PCLK1=32 MHz: 32e6/115200 = 277.8 -> 278 (-0.08%).
    uint32_t usart_brr(uint32_t fck, uint32_t baud)
    {
        return (fck + baud / 2u) / baud;
    }

    void enable_fpu()
    {
        r32(0xE000ED88) |= (0xFu << 20); // CPACR: CP10/CP11 full access
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }

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

    // Bring SYSCLK to 64 MHz on HSI/2 x PLL16. Flash wait states first, then the
    // PLL. If HSI or the PLL never signals ready, leave the reset HSI 8 MHz clock
    // (pclk1_hz stays 8 MHz so the console BRR is still correct) and return.
    void clock_init()
    {
        r32(RCC_CR) |= CR_HSION;
        if (!poll_set(RCC_CR, CR_HSIRDY))
        {
            return; // no HSI (cannot happen at reset) -> stay put
        }

        uint32_t acr = r32(FLASH_ACR);
        acr &= ~ACR_LATENCY_MASK;
        acr |= ACR_LATENCY_2WS | ACR_PRFTBE;
        r32(FLASH_ACR) = acr;

        // PLLSRC/PLLMUL are writable only while the PLL is off (it is at reset).
        uint32_t cfgr = r32(RCC_CFGR);
        cfgr &= ~CFGR_PLL_FIELDS_MASK;
        cfgr |= CFGR_HPRE_DIV1 | CFGR_PPRE1_DIV2 | CFGR_PPRE2_DIV1;
        cfgr |= CFGR_PLLSRC_HSI_DIV2 | CFGR_PLLMUL16;
        r32(RCC_CFGR) = cfgr;

        r32(RCC_CR) |= CR_PLLON;
        if (!poll_set(RCC_CR, CR_PLLRDY))
        {
            return; // PLL failed -> stay on HSI 8 MHz
        }

        uint32_t sw = r32(RCC_CFGR);
        sw &= ~CFGR_SW_MASK;
        sw |= CFGR_SW_PLL;
        r32(RCC_CFGR) = sw;
        for (uint32_t i = 0; i < CLOCK_POLL_LIMIT; i++)
        {
            if ((r32(RCC_CFGR) & CFGR_SWS_MASK) == CFGR_SWS_PLL)
            {
                SystemCoreClock = 64000000u;
                pclk1_hz = 32000000u;
                return;
            }
        }
    }

    void usart2_init()
    {
        r32(RCC_AHBENR) |= AHBENR_IOPAEN;
        r32(RCC_APB1ENR) |= APB1ENR_USART2EN;

        // PA2/PA3 -> AF mode (0b10), AF7 (USART2).
        uint32_t moder = r32(GPIOA_MODER);
        moder &= ~(0xFu << 4);              // clear MODER2/MODER3
        moder |= (0x2u << 4) | (0x2u << 6);
        r32(GPIOA_MODER) = moder;
        uint32_t afrl = r32(GPIOA_AFRL);
        afrl &= ~(0xFFu << 8);             // clear AFRL2/AFRL3
        afrl |= (7u << 8) | (7u << 12);    // AF7
        r32(GPIOA_AFRL) = afrl;

        r32(USART2_CR1) = 0;               // BRR writable only while UE=0
        r32(USART2_BRR) = usart_brr(pclk1_hz, 115200u);
        r32(USART2_CR1) = CR1_UE | CR1_TE | CR1_RE;
    }
}

extern "C"
{

void arch_init(void)
{
    // FPU enabled earlier (Reset_Handler). Clock first (HSI/2 -> PLL -> 64 MHz),
    // then the console derives its BRR from the achieved PCLK1.
    clock_init();
    usart2_init();
    kickos_armv7m_init();
}

void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        while ((r32(USART2_ISR) & ISR_TXE) == 0)
        {
        }
        r32(USART2_TDR) = static_cast<uint8_t>(buf[i]);
    }
}

// Kernel diagnostic LED: LD2 = PB13, active-high. The Nucleo-F302R8 wires LD2 to
// PB13, NOT the usual Nucleo-64 PA5 -- UM1724 documents this LD2 = PA5-or-PB13
// split per target (confirmed against the ST board doc). GPIOB is on the AHB at
// 0x4800_0400 (GPIOA + 0x400); its clock enable is RCC_AHBENR.IOPBEN (bit 18).
void arch_diag_led_init(void)
{
    constexpr uintptr_t GPIOB_MODER = 0x48000400 + 0x00;
    r32(RCC_AHBENR) |= (1u << 18); // IOPBEN (GPIOB)
    uint32_t m = r32(GPIOB_MODER);
    m &= ~(0x3u << 26);            // clear MODER13
    m |= (0x1u << 26);            // general-purpose output
    r32(GPIOB_MODER) = m;
}

void arch_diag_led_set(int on)
{
    constexpr uintptr_t GPIOB_BSRR = 0x48000400 + 0x18;
    if (on)
    {
        r32(GPIOB_BSRR) = 1u << 13;
    }
    else
    {
        r32(GPIOB_BSRR) = 1u << (13 + 16);
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
    enable_fpu();

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
