// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F103C8 ("Blue Pill", Cortex-M3) chip backend. The PLL bring-up, the monotonic clock and
// the buffered console are shared in ../stm32f1f3/chip_stm32f1f3.cc. Registers clean-room from
// RM0008; no vendor HAL/CMSIS. No FPU, no MPU and no watchdog at reset: the reset path is
// C-runtime only.

#include <kickos/arch/arch.h>
#include "crt_tail.h"
#include "pin_guard.h"
#include <kickos/config/limits.h>
#include <kickos/sys/abi.h>

#include "board_pins.h"
#include "family_map.h"
#include "stm32_gpio.h"


#include <stdint.h>

extern "C"
{
    void kickos_armv7m_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

    uint32_t SystemCoreClock = 8000000u; // HSI at reset
}

namespace
{
    using namespace kickos::stm32;

    constexpr uintptr_t RCC_APB2ENR = mmap::RCC_BASE + 0x18;
    constexpr uint32_t APB2ENR_AFIOEN = 1u << 0;
    constexpr uint32_t APB2ENR_USART1EN = 1u << 14;
    constexpr uint32_t APB1ENR_TIM2EN = 1u << 0;
    constexpr uint32_t APB1ENR_TIM3EN = 1u << 1;

    static_assert(KICKOS_BOARD_CONSOLE_BASE == mmap::USART1_BASE,
                  "the board's console is not the USART this backend drives");
    static_assert(KICKOS_BOARD_CONSOLE_TX_SELECT == 0 and KICKOS_BOARD_CONSOLE_RX_SELECT == 0,
                  "this backend writes no AFIO remap, so the console pins are USART1's unremapped pair");

    constexpr uint32_t CR_AF_PUSH_PULL = 0xBu;
    constexpr uint32_t CR_INPUT_FLOATING = 0x4u;
    constexpr uint32_t CR_OUTPUT_2MHZ = 0x2u;

    // func = the raw 4-bit CRL/CRH nibble (0xB = AF push-pull 50 MHz, 0x3 = GP push-pull
    // output 50 MHz, 0x4 = floating input). Default-mapped functions only: a remap needs
    // AFIO_MAPR, and a pull-up/down input (CNF=10) needs an ODR write the nibble cannot carry.
    constexpr uintptr_t GPIO_CRL_OFF = 0x00;
    constexpr uintptr_t GPIO_CRH_OFF = 0x04;
    constexpr uint32_t APB2ENR_IOP_SHIFT = 2u;
    constexpr uint32_t PINMUX_PORT_MAX = 4u; // GPIOA..GPIOE

    void console_pins_init()
    {
        constexpr uintptr_t tx = nibble_reg(KICKOS_BOARD_CONSOLE_TX_PORT_BASE + GPIO_CRL_OFF, KICKOS_BOARD_CONSOLE_TX_BIT);
        constexpr uintptr_t rx = nibble_reg(KICKOS_BOARD_CONSOLE_RX_PORT_BASE + GPIO_CRL_OFF, KICKOS_BOARD_CONSOLE_RX_BIT);
        constexpr uint32_t tx_shift = nibble_shift(KICKOS_BOARD_CONSOLE_TX_BIT);
        constexpr uint32_t rx_shift = nibble_shift(KICKOS_BOARD_CONSOLE_RX_BIT);
        if constexpr (tx == rx)
        {
            rmw(tx, (0xFu << tx_shift) | (0xFu << rx_shift),
                (CR_AF_PUSH_PULL << tx_shift) | (CR_INPUT_FLOATING << rx_shift));
        }
        else
        {
            rmw(tx, 0xFu << tx_shift, CR_AF_PUSH_PULL << tx_shift);
            rmw(rx, 0xFu << rx_shift, CR_INPUT_FLOATING << rx_shift);
        }
    }

    constexpr bool LED_LIT = KICKOS_BOARD_LED_ACTIVE_LOW == 0;

    void timer_clock_init()
    {
        // Boot-order: nothing before arch_init may read the clock. A static ctor
        // (__init_array) calling ktime_now()/arch_clock_now() BusFaults here on
        // the ungated APB1 access.
        reg32(RCC_APB1ENR) |= APB1ENR_TIM2EN | APB1ENR_TIM3EN;

        reg32(mmap::TIM2_BASE + TIM_CR1) = 0;
        reg32(mmap::TIM2_BASE + TIM_PSC) = 0;
        reg32(mmap::TIM2_BASE + TIM_ARR) = 0x0000FFFFu;
        reg32(mmap::TIM2_BASE + TIM_CR2) = 0x2u << 4; // MMS=010: TRGO on update
        reg32(mmap::TIM2_BASE + TIM_EGR) = TIM_EGR_UG;

        // TIM3 counts TIM2 overflows: one 32-bit counter, and TIM3's overflow is its wrap.
        reg32(mmap::TIM3_BASE + TIM_CR1) = 0;
        reg32(mmap::TIM3_BASE + TIM_PSC) = 0;
        reg32(mmap::TIM3_BASE + TIM_ARR) = 0x0000FFFFu;
        reg32(mmap::TIM3_BASE + TIM_SMCR) = (0x1u << 4) | (0x7u << 0); // TS=ITR1, SMS=ext clock 1
        reg32(mmap::TIM3_BASE + TIM_EGR) = TIM_EGR_UG;
        reg32(chip::CLK_TIMER_SR) = ~TIM_SR_UIF;   // drop the UG-induced UIF before arming the IRQ
        reg32(mmap::TIM3_BASE + TIM_DIER) = TIM_DIER_UIE; // wrap observer for the disarmed-timer idle case

        // Slave before master, or a master TRGO edge is missed.
        reg32(mmap::TIM3_BASE + TIM_CR1) = TIM_CR1_CEN;
        reg32(mmap::TIM2_BASE + TIM_CR1) = TIM_CR1_CEN;
        // No arch_irq_clear_pending: a pend latched here (latch-and-coalesce) redelivers
        // one benign kickos_isr_timer tick on enable, which the tickless handler tolerates.
        arch_irq_unmask(chip::CLK_TIMER_IRQ);
    }

    void usart1_init()
    {
        reg32(RCC_APB2ENR) |= (1u << (APB2ENR_IOP_SHIFT + KICKOS_BOARD_CONSOLE_TX_PORT))
                              | (1u << (APB2ENR_IOP_SHIFT + KICKOS_BOARD_CONSOLE_RX_PORT)) | APB2ENR_AFIOEN
                              | APB2ENR_USART1EN;
        console_pins_init();

        reg32(chip::USART_CR1) = 0;
        // USART1 is on APB2 with HPRE=/1 PPRE2=/1, so PCLK2 equals SystemCoreClock at the PLL
        // rate and the degraded reset rate alike; a fixed 72 MHz would garble the degrade path.
        reg32(chip::USART_BRR) = usart_brr(SystemCoreClock, 115200u);
        reg32(chip::USART_CR1) = chip::CR1_UE | chip::CR1_TE | chip::CR1_RE; // TXEIE stays clear
    }
}

extern "C"
{

void arch_init(void)
{
    // Clock first: the console's BRR derives from the final PCLK2.
    if (kickos::stm32::clock_init())
    {
        SystemCoreClock = 72000000u;
    }
    timer_clock_init();
    // Anchor the clock ONCE, from the FINAL rate: the chained counter's LSB increments
    // at TIM2's kernel clock and, with HPRE=/1 and PPRE1 in {/1,/2}, the STM32 APB
    // timer-clock doubler makes that equal HCLK == SystemCoreClock.
    kickos::stm32::clock_anchor(SystemCoreClock);
    usart1_init();
    kickos_armv7m_init();
}

// ST omits the MPU from the STM32F1 Cortex-M3, where a reader expects one: 0 is the
// seam's no-MPU granule (arch.h), not a tuning choice.
size_t arch_mpu_min_region(void)
{
    return 0u;
}

void arch_diag_led_init(void)
{
    constexpr uintptr_t led_cr = nibble_reg(KICKOS_BOARD_LED_PORT_BASE + GPIO_CRL_OFF, KICKOS_BOARD_LED_BIT);
    constexpr uint32_t shift = nibble_shift(KICKOS_BOARD_LED_BIT);
    reg32(RCC_APB2ENR) |= (1u << (APB2ENR_IOP_SHIFT + KICKOS_BOARD_LED_PORT));
    uint32_t v = reg32(led_cr);
    v &= ~(0xFu << shift);
    v |= (CR_OUTPUT_2MHZ << shift);
    reg32(led_cr) = v;
}

void arch_diag_led_set(int on)
{
    constexpr uintptr_t bsrr = KICKOS_BOARD_LED_PORT_BASE + 0x10;
    if (on)
    {
        reg32(bsrr) = bsrr_word(KICKOS_BOARD_LED_BIT, LED_LIT);
    }
    else
    {
        reg32(bsrr) = bsrr_word(KICKOS_BOARD_LED_BIT, not LED_LIT);
    }
}

// Validate BEFORE gating a clock: a gate-then-fail path would leak an enabled clock.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port > PINMUX_PORT_MAX or pin > 15u)
    {
        return -KOS_EINVAL;
    }
    if (kickos::board_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    // AFIOEN is needed for any alternate-function pin.
    reg32(RCC_APB2ENR) |= APB2ENR_AFIOEN | (1u << (APB2ENR_IOP_SHIFT + port));
    uintptr_t const base = mmap::GPIOA_BASE + port * mmap::GPIO_STRIDE;
    uintptr_t cr = base + GPIO_CRL_OFF;
    uint32_t shift = pin * 4u;
    if (pin >= 8u)
    {
        cr = base + GPIO_CRH_OFF;
        shift = (pin - 8u) * 4u;
    }
    uint32_t v = reg32(cr);
    v &= ~(0xFu << shift);
    v |= (func & 0xFu) << shift;
    reg32(cr) = v;
    return 0;
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
    kickos_crt_tail();
}

}
