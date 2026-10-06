// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F103C8 ("Blue Pill", Cortex-M3) chip backend: what this part does NOT share with
// the rest of the STM32 F0/F1/F3 family. The PLL bring-up, the monotonic clock and the
// buffered console live in ../stm32f1f3/chip_stm32f1f3.cc off family_map.h; here are the
// timer chain, the older CRL/CRH GPIO model, the diagnostic LED and the reset path.
//
// Registers are clean-room from RM0008; hand-rolled, no vendor HAL/CMSIS. Console = USART1, on
// the pins the board file names. No FPU, no MPU, and no watchdog runs at reset, so the reset path
// is just C-runtime.

#include <kickos/arch/arch.h>
#include <kickos/arch/pin_guard.h>
#include <kickos/config/limits.h>
#include <kickos/sys/abi.h> // KOS_E* codes for arch_pinmux_set

#include "board_pins.h"
#include "family_map.h"
#include "stm32_gpio.h"


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

    // The CRL/CRH nibble: AF push-pull 50 MHz drives TX, a floating input takes RX, and a
    // push-pull output at 2 MHz drives the LED.
    constexpr uint32_t CR_AF_PUSH_PULL = 0xBu;
    constexpr uint32_t CR_INPUT_FLOATING = 0x4u;
    constexpr uint32_t CR_OUTPUT_2MHZ = 0x2u;

    // --- Pin-mux (KOS_SYS_PINMUX_SET) -------------------------------------------
    // GPIO ports: GPIOA + port * GPIO_STRIDE. RCC_APB2ENR IOPxEN = bit (2+port); AFIOEN
    // (bit 0) also gated. func = the raw 4-bit CRL/CRH nibble (0xB = AF push-pull
    // 50 MHz, 0x3 = GP push-pull output 50 MHz, 0x4 = floating input); the nibble
    // sits at (pin%8)*4 in CRL (pin<8) / CRH (pin>=8). Sufficient for the
    // default-mapped peripherals only: alternate-function REMAP goes through
    // AFIO_MAPR (per-peripheral, out of scope here), and a pull-up/down input
    // (CNF=10) additionally needs an ODR write the 4-bit nibble cannot carry.
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

#define KICKOS_RESERVED_RUN(port_base, first, last) \
    or (mmap::GPIOA_BASE + port * mmap::GPIO_STRIDE == (port_base) and pin >= (first) and pin <= (last))
    constexpr bool f1_pin_kernel_owned(uint32_t port, uint32_t pin)
    {
        return (port == KICKOS_BOARD_CONSOLE_TX_PORT and pin == KICKOS_BOARD_CONSOLE_TX_BIT)
               or (port == KICKOS_BOARD_CONSOLE_RX_PORT and pin == KICKOS_BOARD_CONSOLE_RX_BIT)
               or (port == KICKOS_BOARD_LED_PORT and pin == KICKOS_BOARD_LED_BIT)
                   KICKOS_BOARD_RESERVED_RUNS(KICKOS_RESERVED_RUN);
    }
#undef KICKOS_RESERVED_RUN

#define KICKOS_KERNEL_PIN(port_base, bit) or (mmap::GPIOA_BASE + port * mmap::GPIO_STRIDE == (port_base) and pin == (bit))
    constexpr bool f1_pin_listed(uint32_t port, uint32_t pin)
    {
        return false KICKOS_BOARD_KERNEL_PINS(KICKOS_KERNEL_PIN);
    }
#undef KICKOS_KERNEL_PIN
    static_assert(kickos::refuses_exactly(f1_pin_kernel_owned, f1_pin_listed, PINMUX_PORT_MAX + 1u, 16u),
                  "arch_pinmux_set refuses other pins than the board's kernel pins");

    constexpr bool LED_LIT = KICKOS_BOARD_LED_ACTIVE_LOW == 0;

    void timer_clock_init()
    {
        // Boot-order: nothing before arch_init may read the clock. A static ctor
        // (__init_array) calling ktime_now()/arch_clock_now() BusFaults here on
        // the ungated APB1 access.
        reg32(RCC_APB1ENR) |= APB1ENR_TIM2EN | APB1ENR_TIM3EN;

        // TIM2 master: free-run 16-bit, emit TRGO on each overflow.
        reg32(mmap::TIM2_BASE + TIM_CR1) = 0;
        reg32(mmap::TIM2_BASE + TIM_PSC) = 0;
        reg32(mmap::TIM2_BASE + TIM_ARR) = 0x0000FFFFu;
        reg32(mmap::TIM2_BASE + TIM_CR2) = 0x2u << 4; // MMS=010: TRGO on update
        reg32(mmap::TIM2_BASE + TIM_EGR) = TIM_EGR_UG;

        // TIM3 slave: clocked by TIM2's TRGO (ITR1), free-run 16-bit. Its overflow
        // (the 32-bit chain wrap) drives the idle wrap observer.
        reg32(mmap::TIM3_BASE + TIM_CR1) = 0;
        reg32(mmap::TIM3_BASE + TIM_PSC) = 0;
        reg32(mmap::TIM3_BASE + TIM_ARR) = 0x0000FFFFu;
        reg32(mmap::TIM3_BASE + TIM_SMCR) = (0x1u << 4) | (0x7u << 0); // TS=ITR1, SMS=ext clock 1
        reg32(mmap::TIM3_BASE + TIM_EGR) = TIM_EGR_UG;
        reg32(chip::CLK_TIMER_SR) = ~TIM_SR_UIF;   // drop the UG-induced UIF before arming the IRQ
        reg32(mmap::TIM3_BASE + TIM_DIER) = TIM_DIER_UIE; // wrap observer for the disarmed-timer idle case

        // Enable the slave first so no master TRGO edge is missed, then the master.
        reg32(mmap::TIM3_BASE + TIM_CR1) = TIM_CR1_CEN;
        reg32(mmap::TIM2_BASE + TIM_CR1) = TIM_CR1_CEN;
        // No arch_irq_clear_pending: a pend latched here (latch-and-coalesce) redelivers
        // one benign kickos_isr_timer tick on enable, which the tickless handler tolerates.
        arch_irq_unmask(chip::CLK_TIMER_IRQ); // NVIC enable in the maskable device band
    }

    void usart1_init()
    {
        reg32(RCC_APB2ENR) |= (1u << (APB2ENR_IOP_SHIFT + KICKOS_BOARD_CONSOLE_TX_PORT))
                              | (1u << (APB2ENR_IOP_SHIFT + KICKOS_BOARD_CONSOLE_RX_PORT)) | APB2ENR_AFIOEN
                              | APB2ENR_USART1EN;
        console_pins_init();

        reg32(chip::USART_CR1) = 0; // disable while configuring
        // USART1 is on APB2 and clock_init leaves HPRE=/1 PPRE2=/1, so PCLK2 equals
        // SystemCoreClock at the PLL rate and at the degraded reset rate alike. Deriving the
        // divisor from a fixed 72 MHz instead would garble the console on the degrade path.
        reg32(chip::USART_BRR) = usart_brr(SystemCoreClock, 115200u);
        reg32(chip::USART_CR1) = chip::CR1_UE | chip::CR1_TE | chip::CR1_RE; // TXEIE stays clear
    }
}

extern "C"
{

void arch_init(void)
{
    // Clock first (HSE/PLL -> 72 MHz), then the console derives its BRR from PCLK2.
    if (kickos::stm32::clock_init())
    {
        SystemCoreClock = 72000000u;
    }
    timer_clock_init(); // monotonic time base: the required arch_clock_now source
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

// One-shot pin-function config (KOS_SYS_PINMUX_SET). func = the raw 4-bit CRL/CRH
// nibble. Validate range + kernel-owned BEFORE gating a clock or touching a
// register (a gate-then-fail path would leak an enabled clock).
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port > PINMUX_PORT_MAX or pin > 15u)
    {
        return -KOS_EINVAL;
    }
    if (f1_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    // AFIOEN is needed for any alternate-function pin; the port clock is per-port.
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
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
