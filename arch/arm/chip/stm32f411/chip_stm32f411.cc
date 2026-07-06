// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 (STM32F411E-DISCO, Cortex-M4F) chip backend. Registers are clean-room
// from RM0383; hand-rolled, no vendor HAL/CMSIS, consistent with the arch layer.
//
// M1 scope: privilege + SVC, no hardware MPU. Clocking: HSE crystal (8 MHz on the
// F411E-DISCO) -> main PLL -> 84 MHz SYSCLK for an accurate, full-speed core (the
// HSI RC is too imprecise for reliable 115200 UART). clock_init() runs first in
// arch_init and bounded-polls every ready flag, so a dead/missing crystal degrades
// to the reset-default HSI 16 MHz instead of hanging (the BRR is recomputed from
// whichever APB1 clock we end up on). Console = USART2 on PA2(TX)/PA3(RX), AF7,
// polled TX. STM32 keeps peripheral clocks running in WFI, so no TX drain is needed
// (unlike the XMC). STM32 has no watchdog running at reset (unlike the K64F), so
// the reset path is FPU + C-runtime + clocks.
//
// NOT run in this environment (no F411 model here); verified by build + image
// inspection. Flash (ST-LINK/openocd) to confirm; apps/blink toggles an onboard
// LED (PD12) for a no-UART smoke test.

#include <kickos/arch/arch.h>
#include <kickos/board_config.h> // per-board HSE freq + LED pin (Disco vs Black Pill)

#include <stdint.h>

// Board defaults if a board_config.h omits them (keeps a standalone compile sane;
// the shipped boards define all of these). Disco values.
#ifndef KICKOS_HSE_HZ
#define KICKOS_HSE_HZ 8000000
#endif
#ifndef KICKOS_LED_GPIO
#define KICKOS_LED_GPIO 0x40020C00
#endif
#ifndef KICKOS_LED_RCC_AHB1_BIT
#define KICKOS_LED_RCC_AHB1_BIT 3
#endif
#ifndef KICKOS_LED_PIN
#define KICKOS_LED_PIN 12
#endif
#ifndef KICKOS_LED_ACTIVE_LOW
#define KICKOS_LED_ACTIVE_LOW 0
#endif

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

    uint32_t SystemCoreClock = 16000000u; // updated by clock_init(); HSI on fallback
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // RCC (RM0383 §6): clock control + PLL + peripheral clock enables.
    constexpr uintptr_t RCC_BASE = 0x40023800;
    constexpr uintptr_t RCC_CR = RCC_BASE + 0x00;      // §6.3.1 (RM lines 5129-5194)
    constexpr uintptr_t RCC_PLLCFGR = RCC_BASE + 0x04; // §6.3.2 (RM lines 5227-5324)
    constexpr uintptr_t RCC_CFGR = RCC_BASE + 0x08;    // §6.3.3 (RM lines 5333-5474)
    constexpr uintptr_t RCC_AHB1ENR = RCC_BASE + 0x30;
    constexpr uintptr_t RCC_APB1ENR = RCC_BASE + 0x40;
    constexpr uint32_t AHB1ENR_GPIOAEN = 1u << 0;
    constexpr uint32_t APB1ENR_USART2EN = 1u << 17;

    // RCC_CR flags (RM lines 5157-5194).
    constexpr uint32_t CR_HSEON = 1u << 16;
    constexpr uint32_t CR_HSERDY = 1u << 17;
    constexpr uint32_t CR_PLLON = 1u << 24;
    constexpr uint32_t CR_PLLRDY = 1u << 25;

    // RCC_CFGR: system-clock switch + bus prescalers (RM lines 5419-5474).
    constexpr uint32_t CFGR_SW_PLL = 0x2u << 0;   // SW=10: PLL as SYSCLK
    constexpr uint32_t CFGR_SWS_PLL = 0x2u << 2;  // SWS=10: PLL is SYSCLK (readback)
    constexpr uint32_t CFGR_HPRE_DIV1 = 0x0u << 4;  // AHB  = SYSCLK/1  = 84 MHz
    constexpr uint32_t CFGR_PPRE1_DIV2 = 0x4u << 10; // APB1 = HCLK/2   = 42 MHz (<=42)
    constexpr uint32_t CFGR_PPRE2_DIV1 = 0x0u << 13; // APB2 = HCLK/1   = 84 MHz (<=84)

    // Main PLL from HSE -> 84 MHz (RM lines 5232-5324). PLLM is chosen per board
    // to make VCO_in exactly 1 MHz regardless of the crystal (Disco 8 MHz -> PLLM
    // 8; Black Pill 25 MHz -> PLLM 25), so PLLN/PLLP/PLLQ are board-independent:
    //   VCO_in  = HSE / PLLM  = 1 MHz              (1..2 MHz, RM line 5316)
    //   VCO_out = VCO_in * PLLN = 1 MHz * 336 = 336 MHz (100..432 MHz, RM line 5293)
    //   SYSCLK  = VCO_out / PLLP = 336 / 4   = 84 MHz  (<=100 MHz, RM line 5280)
    //   PLL48   = VCO_out / PLLQ = 336 / 7   = 48 MHz  (USB/SDIO, RM line 5251)
    constexpr uint32_t PLLM = KICKOS_HSE_HZ / 1000000u;
    constexpr uint32_t PLLN = 336u;
    constexpr uint32_t PLLQ = 7u;
    constexpr uint32_t PLLCFGR_PLLSRC_HSE = 1u << 22;  // HSE as PLL entry (RM line 5266)
    constexpr uint32_t PLLCFGR_PLLP_DIV4 = 0x1u << 16; // PLLP=01 -> /4 (RM line 5283)
    constexpr uint32_t PLLCFGR_VALUE =
        (PLLQ << 24) | PLLCFGR_PLLSRC_HSE | PLLCFGR_PLLP_DIV4 | (PLLN << 6) | PLLM;

    constexpr uint32_t HSE_HZ = KICKOS_HSE_HZ;
    constexpr uint32_t SYSCLK_PLL_HZ = 84000000u;
    constexpr uint32_t PCLK1_PLL_HZ = 42000000u; // APB1 = 84/2

    // FLASH (RM §3.8.1, lines 2784-2828). At 3.3 V (2.7-3.6 V) and HCLK=84 MHz,
    // Table 5 (RM line 2066: 64 < HCLK <= 90) requires 2 wait states. Prefetch +
    // I/D caches (ART) restore ~0-WS effective execution (RM line 2121).
    constexpr uintptr_t FLASH_ACR = 0x40023C00;
    constexpr uint32_t ACR_LATENCY_2WS = 0x2u << 0;
    constexpr uint32_t ACR_PRFTEN = 1u << 8;
    constexpr uint32_t ACR_ICEN = 1u << 9;
    constexpr uint32_t ACR_DCEN = 1u << 10;

    // Bounded so a dead/missing crystal degrades to HSI instead of hanging boot
    // forever (a silent hang leaves no UART/LED sign of life). The cap is far
    // longer than any legitimate wait (HSE startup is well under 1 ms).
    constexpr uint32_t POLL_TIMEOUT = 1000000u;

    // APB1 clock the console runs on; set by clock_init(). Defaults to the HSI
    // fallback (SYSCLK=HCLK=PCLK1=16 MHz at reset) so the UART still works if the
    // crystal never comes up.
    uint32_t pclk1_hz = 16000000u;

    // GPIOA (§8): MODER (2b/pin) + AFRL (4b/pin, pins 0-7). USART2 = AF7.
    constexpr uintptr_t GPIOA_BASE = 0x40020000;
    constexpr uintptr_t GPIOA_MODER = GPIOA_BASE + 0x00;
    constexpr uintptr_t GPIOA_AFRL = GPIOA_BASE + 0x20;

    // USART2 (§19), classic SR/DR. On APB1 (42 MHz on PLL, 16 MHz on HSI fallback).
    constexpr uintptr_t USART2_BASE = 0x40004400;
    constexpr uintptr_t USART2_SR = USART2_BASE + 0x00;
    constexpr uintptr_t USART2_DR = USART2_BASE + 0x04;
    constexpr uintptr_t USART2_BRR = USART2_BASE + 0x08;
    constexpr uintptr_t USART2_CR1 = USART2_BASE + 0x0C;
    constexpr uint32_t SR_TXE = 1u << 7;
    constexpr uint32_t CR1_UE = 1u << 13;
    constexpr uint32_t CR1_TE = 1u << 3;
    constexpr uint32_t CR1_RE = 1u << 2;
    constexpr uint32_t BAUD_115200 = 115200u;

    // OVER8=0: baud = fPCLK1 / (16 * USARTDIV) (RM lines 28373-28378). The BRR
    // register value equals 16*USARTDIV = fPCLK1/baud, with BRR[15:4]=mantissa and
    // BRR[3:0]=fraction/16 (RM lines 27814-27830), so round fPCLK1/baud to nearest:
    //   PLL   : 42e6/115200 = 364.58 -> 365 = 0x16D (=> 42e6/(16*22.8125)=115068, -0.11%)
    //   HSI   : 16e6/115200 = 138.89 -> 139 = 0x8B
    uint32_t usart_brr(uint32_t fpclk1, uint32_t baud)
    {
        return (fpclk1 + baud / 2u) / baud;
    }

    bool wait_mask(uintptr_t addr, uint32_t mask)
    {
        for (uint32_t i = 0; i < POLL_TIMEOUT; i++)
        {
            if ((r32(addr) & mask) == mask)
            {
                return true;
            }
        }
        return false;
    }

    // HSE crystal -> PLL -> 84 MHz. Every ready flag is bounded-polled; on any
    // failure we leave the reset-default HSI 16 MHz selected and pclk1_hz at 16 MHz.
    void clock_init()
    {
        // Flash access time MUST be widened before the core runs faster, else the
        // first over-speed instruction fetch faults (RM lines 2048-2052, 2079).
        r32(FLASH_ACR) = ACR_LATENCY_2WS | ACR_PRFTEN | ACR_ICEN | ACR_DCEN;

        r32(RCC_CR) |= CR_HSEON;
        if (!wait_mask(RCC_CR, CR_HSERDY))
        {
            return; // no crystal: stay on HSI 16 MHz
        }

        // PLL config bits are writable only while PLL is off (RM lines 5250, 5279).
        r32(RCC_PLLCFGR) = PLLCFGR_VALUE;

        // Set bus prescalers before the fast clock is live so APB1<=42 / APB2<=84
        // are never briefly exceeded when SYSCLK switches to the PLL.
        r32(RCC_CFGR) = CFGR_HPRE_DIV1 | CFGR_PPRE1_DIV2 | CFGR_PPRE2_DIV1;

        r32(RCC_CR) |= CR_PLLON;
        if (!wait_mask(RCC_CR, CR_PLLRDY))
        {
            return; // PLL never locked: stay on HSI 16 MHz
        }

        r32(RCC_CFGR) = (r32(RCC_CFGR) & ~0x3u) | CFGR_SW_PLL;
        if (!wait_mask(RCC_CFGR, CFGR_SWS_PLL)) // SWS reads back the active source
        {
            return; // switch did not take: HSI still drives SYSCLK
        }

        SystemCoreClock = SYSCLK_PLL_HZ;
        pclk1_hz = PCLK1_PLL_HZ;
    }

    void enable_fpu()
    {
        r32(0xE000ED88) |= (0xFu << 20); // CPACR: CP10/CP11 full access
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }

    void usart2_init()
    {
        r32(RCC_AHB1ENR) |= AHB1ENR_GPIOAEN;
        r32(RCC_APB1ENR) |= APB1ENR_USART2EN;

        // PA2/PA3 -> alternate-function mode (0b10), AF7 (USART2).
        uint32_t moder = r32(GPIOA_MODER);
        moder &= ~(0xFu << 4);                 // clear MODER2/MODER3 (bits 4..7)
        moder |= (0x2u << 4) | (0x2u << 6);    // AF mode for PA2, PA3
        r32(GPIOA_MODER) = moder;
        uint32_t afrl = r32(GPIOA_AFRL);
        afrl &= ~(0xFFu << 8);                 // clear AFRL2/AFRL3 (bits 8..15)
        afrl |= (7u << 8) | (7u << 12);        // AF7 for PA2, PA3
        r32(GPIOA_AFRL) = afrl;

        r32(USART2_CR1) = 0;         // disable while configuring (OVER8=0)
        r32(USART2_BRR) = usart_brr(pclk1_hz, BAUD_115200);
        r32(USART2_CR1) = CR1_UE | CR1_TE | CR1_RE;
    }
}

extern "C"
{

void arch_init(void)
{
    // FPU is enabled earlier (Reset_Handler, before C++ ctors). Bring the core up
    // on the HSE crystal + PLL first, then configure the console at the resulting
    // APB1 clock (clock_init leaves us on HSI 16 MHz if the crystal is absent).
    clock_init();
    usart2_init();
    kickos_armv7m_init();
}

void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        while ((r32(USART2_SR) & SR_TXE) == 0)
        {
        }
        r32(USART2_DR) = static_cast<uint8_t>(buf[i]);
    }
}

// Kernel diagnostic LED: the pin/port/polarity are board facts (KICKOS_LED_*,
// from board_config.h) so one stm32f411 backend drives the Disco's PD12
// (active-high) and the Black Pill's PC13 (active-low) unchanged.
void arch_diag_led_init(void)
{
    r32(RCC_AHB1ENR) |= (1u << KICKOS_LED_RCC_AHB1_BIT);
    uint32_t m = r32(KICKOS_LED_GPIO + 0x00);       // MODER
    m &= ~(0x3u << (KICKOS_LED_PIN * 2));
    m |= (0x1u << (KICKOS_LED_PIN * 2));            // general-purpose output
    r32(KICKOS_LED_GPIO + 0x00) = m;
}

void arch_diag_led_set(int on)
{
    constexpr uintptr_t bsrr = KICKOS_LED_GPIO + 0x18; // BSRR: [15:0]=set, [31:16]=reset
    bool high = (on != 0);
#if KICKOS_LED_ACTIVE_LOW
    high = !high; // lit when driven low
#endif
    if (high)
    {
        r32(bsrr) = 1u << KICKOS_LED_PIN;
    }
    else
    {
        r32(bsrr) = 1u << (KICKOS_LED_PIN + 16);
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
    enable_fpu(); // before any code that a hard-float ABI might emit FP into

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
