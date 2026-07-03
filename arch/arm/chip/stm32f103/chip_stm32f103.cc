// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F103C8 ("Blue Pill", Cortex-M3) chip backend. Registers are clean-room
// from RM0008; hand-rolled, no vendor HAL/CMSIS, consistent with the arch layer.
//
// M1 scope: privilege + SVC, no hardware MPU. Clocking is minimal -- NO PLL: the
// HSI 8 MHz RC is on and selected as SYSCLK at reset, so SYSCLK = HCLK = PCLK2 =
// 8 MHz with zero config. Console = USART1 on PA9(TX)/PA10(RX), polled TX. F103
// uses the older CRL/CRH GPIO model (not MODER/AFR) and has no FPU. No watchdog
// runs at reset, so the reset path is just C-runtime.
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

    uint32_t SystemCoreClock = 8000000u; // HSI, no PLL
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // RCC (RM0008 §7). HSI needs no setup; just gate on the peripheral clocks.
    constexpr uintptr_t RCC_BASE = 0x40021000;
    constexpr uintptr_t RCC_APB2ENR = RCC_BASE + 0x18;
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

    // USART1 (§27), classic SR/DR. On APB2 (8 MHz).
    constexpr uintptr_t USART1_BASE = 0x40013800;
    constexpr uintptr_t USART1_SR = USART1_BASE + 0x00;
    constexpr uintptr_t USART1_DR = USART1_BASE + 0x04;
    constexpr uintptr_t USART1_BRR = USART1_BASE + 0x08;
    constexpr uintptr_t USART1_CR1 = USART1_BASE + 0x0C;
    constexpr uint32_t SR_TXE = 1u << 7;
    constexpr uint32_t CR1_UE = 1u << 13;
    constexpr uint32_t CR1_TE = 1u << 3;
    constexpr uint32_t CR1_RE = 1u << 2;
    // USARTDIV = 8e6/(16*115200) = 4.34 -> mantissa 4, fraction 5 (+0.64% error).
    constexpr uint32_t BRR_115200 = (4u << 4) | 5u; // 0x45

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
    // HSI is already the system clock; only the console needs setup. No FPU (M3).
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
