// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F302R8 (Nucleo-F302R8, Cortex-M4F) chip backend. Registers clean-room from
// RM0365; hand-rolled, no vendor HAL. Versus the F411: the F3 puts the GPIO ports
// on AHB at 0x4800_0000 (not 0x4002_0000), the RCC block is at 0x4002_1000, and
// the USART is the NEWER model (ISR/TDR, not SR/DR).
//
// M1 scope: privilege + SVC, no MPU. No PLL -- HSI 8 MHz is the reset system
// clock (SYSCLK=HCLK=PCLK=8 MHz). Console = USART2 on PA2/PA3 (AF7, the ST-LINK
// VCP), polled TX. No watchdog runs at reset. Build-only here; flash to confirm,
// or watch the LD2 (PA5) blink from apps/blink.

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

    uint32_t SystemCoreClock = 8000000u; // HSI, no PLL
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // RCC (RM0365 §9): F3 layout, base 0x40021000.
    constexpr uintptr_t RCC_BASE = 0x40021000;
    constexpr uintptr_t RCC_AHBENR = RCC_BASE + 0x14;
    constexpr uintptr_t RCC_APB1ENR = RCC_BASE + 0x1C;
    constexpr uint32_t AHBENR_IOPAEN = 1u << 17; // GPIOA (ports are on AHB)
    constexpr uint32_t APB1ENR_USART2EN = 1u << 17;

    // GPIOA on AHB at 0x48000000 (§11). MODER 2b/pin, AFRL 4b/pin.
    constexpr uintptr_t GPIOA_BASE = 0x48000000;
    constexpr uintptr_t GPIOA_MODER = GPIOA_BASE + 0x00;
    constexpr uintptr_t GPIOA_AFRL = GPIOA_BASE + 0x20;

    // USART2 (§29), NEW model. On APB1 (8 MHz).
    constexpr uintptr_t USART2_BASE = 0x40004400;
    constexpr uintptr_t USART2_CR1 = USART2_BASE + 0x00;
    constexpr uintptr_t USART2_BRR = USART2_BASE + 0x0C;
    constexpr uintptr_t USART2_ISR = USART2_BASE + 0x1C;
    constexpr uintptr_t USART2_TDR = USART2_BASE + 0x28;
    constexpr uint32_t ISR_TXE = 1u << 7;
    constexpr uint32_t CR1_UE = 1u << 0;
    constexpr uint32_t CR1_TE = 1u << 3;
    constexpr uint32_t CR1_RE = 1u << 2;
    constexpr uint32_t BRR_115200 = 0x45; // 8e6/115200 = 69 (OVER8=0: BRR=fck/baud)

    void enable_fpu()
    {
        r32(0xE000ED88) |= (0xFu << 20); // CPACR: CP10/CP11 full access
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
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
        r32(USART2_BRR) = BRR_115200;
        r32(USART2_CR1) = CR1_UE | CR1_TE | CR1_RE;
    }
}

extern "C"
{

void arch_init(void)
{
    // FPU enabled earlier (Reset_Handler). HSI is already the system clock.
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
