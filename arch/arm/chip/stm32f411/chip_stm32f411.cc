// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 (STM32F411E-DISCO, Cortex-M4F) chip backend. Registers are clean-room
// from RM0383; hand-rolled, no vendor HAL/CMSIS, consistent with the arch layer.
//
// M1 scope: privilege + SVC, no hardware MPU. Clocking is minimal -- NO PLL: the
// HSI 16 MHz RC is already on and selected as SYSCLK at reset, so SYSCLK = HCLK =
// PCLK = 16 MHz with zero config (precise UART baud, no PLL sequencing risk).
// Console = USART2 on PA2(TX)/PA3(RX), AF7, polled TX. STM32 has no watchdog
// running at reset (unlike the K64F), so the reset path is FPU + C-runtime.
//
// NOT run in this environment (no F411 model here); verified by build + image
// inspection. Flash (ST-LINK/openocd) to confirm; apps/blink toggles an onboard
// LED (PD12) for a no-UART smoke test.

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

    uint32_t SystemCoreClock = 16000000u; // HSI, no PLL
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // RCC (RM0383 §6): peripheral clock enables. HSI needs no setup.
    constexpr uintptr_t RCC_BASE = 0x40023800;
    constexpr uintptr_t RCC_AHB1ENR = RCC_BASE + 0x30;
    constexpr uintptr_t RCC_APB1ENR = RCC_BASE + 0x40;
    constexpr uint32_t AHB1ENR_GPIOAEN = 1u << 0;
    constexpr uint32_t APB1ENR_USART2EN = 1u << 17;

    // GPIOA (§8): MODER (2b/pin) + AFRL (4b/pin, pins 0-7). USART2 = AF7.
    constexpr uintptr_t GPIOA_BASE = 0x40020000;
    constexpr uintptr_t GPIOA_MODER = GPIOA_BASE + 0x00;
    constexpr uintptr_t GPIOA_AFRL = GPIOA_BASE + 0x20;

    // USART2 (§19), classic SR/DR. On APB1 (16 MHz).
    constexpr uintptr_t USART2_BASE = 0x40004400;
    constexpr uintptr_t USART2_SR = USART2_BASE + 0x00;
    constexpr uintptr_t USART2_DR = USART2_BASE + 0x04;
    constexpr uintptr_t USART2_BRR = USART2_BASE + 0x08;
    constexpr uintptr_t USART2_CR1 = USART2_BASE + 0x0C;
    constexpr uint32_t SR_TXE = 1u << 7;
    constexpr uint32_t CR1_UE = 1u << 13;
    constexpr uint32_t CR1_TE = 1u << 3;
    constexpr uint32_t CR1_RE = 1u << 2;
    // USARTDIV = 16e6/(16*115200) = 8.6875 -> mantissa 8, fraction 11 (RM Table 75).
    constexpr uint32_t BRR_115200 = (8u << 4) | 11u; // 0x8B

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
        r32(USART2_BRR) = BRR_115200;
        r32(USART2_CR1) = CR1_UE | CR1_TE | CR1_RE;
    }
}

extern "C"
{

void arch_init(void)
{
    // FPU is enabled earlier (Reset_Handler, before C++ ctors). HSI is already the
    // system clock, so only the peripheral console needs setup.
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
