// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F302R8 (Nucleo-F302R8, Cortex-M4F) chip backend: what this part does NOT share
// with the rest of the STM32 F0/F1/F3 family. The PLL bring-up, the monotonic clock and
// the buffered console live in ../stm32f1f3/chip_stm32f1f3.cc off family_map.h; here are
// the free-running timer, the AHB GPIO ports, the diagnostic LED and the reset path.
//
// Registers clean-room from RM0365; hand-rolled, no vendor HAL. Versus the F411: the F3
// puts the GPIO ports on AHB at 0x4800_0000 (not 0x4002_0000) and its USART is the newer
// ISR/TDR model. Scope: privilege + SVC, no MPU. Console = USART2, on the pins the board file
// names. No watchdog runs at reset.

#include <kickos/arch/arch.h>
#include <kickos/arch/pin_guard.h>
#include <kickos/config/limits.h>
#include <kickos/sys/abi.h> // KOS_E* codes for arch_pinmux_set

#include "board_pins.h"
#include "family_map.h"
#include "regs.h" // arch/arm/common: kickos_armv7m_enable_fpu
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

    uint32_t SystemCoreClock = 8000000u; // reset HSI; clock_init() lifts to 64 MHz on PLL
}

namespace
{
    using namespace kickos::stm32;

    // APB1 clock feeding USART2. Reset HSI => PCLK1 = 8 MHz; clock_init sets 32 MHz.
    uint32_t pclk1_hz = 8000000u;

    constexpr uintptr_t RCC_AHBENR = mmap::RCC_BASE + 0x14;
    constexpr uint32_t APB1ENR_USART2EN = 1u << 17;
    constexpr uint32_t APB1ENR_TIM2EN = 1u << 0;

    static_assert(KICKOS_BOARD_CONSOLE_BASE == mmap::USART2_BASE,
                  "the board's console is not the USART this backend drives");

    // --- Pin-mux (KOS_SYS_PINMUX_SET) -------------------------------------------
    // GPIO ports are on AHB: GPIOA + port*0x400 (A=0..F=5). RCC_AHBENR IOPxEN =
    // bit (17+port). func: bits[1:0] = MODER field written verbatim (00 in, 01 out,
    // 10 AF, 11 analog); bits[7:4] = AF number, into AFRL (pin<8) / AFRH (pin>=8).
    // OTYPER/OSPEEDR/PUPDR stay at their reset values.
    constexpr uintptr_t GPIO_MODER_OFF = 0x00;
    constexpr uintptr_t GPIO_AFRL_OFF = 0x20;
    constexpr uintptr_t GPIO_AFRH_OFF = 0x24;
    constexpr uint32_t AHBENR_IOP_SHIFT = 17u;
    constexpr uint32_t PINMUX_PORT_MAX = 5u; // GPIOA..GPIOF
    constexpr uint32_t MODER_OUTPUT = 0x1u;
    constexpr uint32_t MODER_AF = 0x2u;

    void console_pins_init()
    {
        constexpr uintptr_t tx = KICKOS_BOARD_CONSOLE_TX_PORT_BASE;
        constexpr uintptr_t rx = KICKOS_BOARD_CONSOLE_RX_PORT_BASE;
        constexpr uint32_t tx_pin = KICKOS_BOARD_CONSOLE_TX_BIT;
        constexpr uint32_t rx_pin = KICKOS_BOARD_CONSOLE_RX_BIT;
        constexpr uintptr_t tx_afr = nibble_reg(tx + GPIO_AFRL_OFF, tx_pin);
        constexpr uintptr_t rx_afr = nibble_reg(rx + GPIO_AFRL_OFF, rx_pin);
        constexpr uint32_t tx_af = nibble_shift(tx_pin);
        constexpr uint32_t rx_af = nibble_shift(rx_pin);
        if constexpr (tx == rx)
        {
            rmw(tx + GPIO_MODER_OFF, (0x3u << (tx_pin * 2u)) | (0x3u << (rx_pin * 2u)),
                (MODER_AF << (tx_pin * 2u)) | (MODER_AF << (rx_pin * 2u)));
        }
        else
        {
            rmw(tx + GPIO_MODER_OFF, 0x3u << (tx_pin * 2u), MODER_AF << (tx_pin * 2u));
            rmw(rx + GPIO_MODER_OFF, 0x3u << (rx_pin * 2u), MODER_AF << (rx_pin * 2u));
        }
        if constexpr (tx_afr == rx_afr)
        {
            rmw(tx_afr, (0xFu << tx_af) | (0xFu << rx_af),
                (uint32_t{KICKOS_BOARD_CONSOLE_TX_SELECT} << tx_af) | (uint32_t{KICKOS_BOARD_CONSOLE_RX_SELECT} << rx_af));
        }
        else
        {
            rmw(tx_afr, 0xFu << tx_af, uint32_t{KICKOS_BOARD_CONSOLE_TX_SELECT} << tx_af);
            rmw(rx_afr, 0xFu << rx_af, uint32_t{KICKOS_BOARD_CONSOLE_RX_SELECT} << rx_af);
        }
    }

#define KICKOS_RESERVED_RUN(port_base, first, last) \
    or (mmap::GPIOA_BASE + port * mmap::GPIO_STRIDE == (port_base) and pin >= (first) and pin <= (last))
    constexpr bool f3_pin_kernel_owned(uint32_t port, uint32_t pin)
    {
        return (port == KICKOS_BOARD_CONSOLE_TX_PORT and pin == KICKOS_BOARD_CONSOLE_TX_BIT)
               or (port == KICKOS_BOARD_CONSOLE_RX_PORT and pin == KICKOS_BOARD_CONSOLE_RX_BIT)
               or (port == KICKOS_BOARD_LED_PORT and pin == KICKOS_BOARD_LED_BIT)
                   KICKOS_BOARD_RESERVED_RUNS(KICKOS_RESERVED_RUN);
    }
#undef KICKOS_RESERVED_RUN

#define KICKOS_KERNEL_PIN(port_base, bit) or (mmap::GPIOA_BASE + port * mmap::GPIO_STRIDE == (port_base) and pin == (bit))
    constexpr bool f3_pin_listed(uint32_t port, uint32_t pin)
    {
        return false KICKOS_BOARD_KERNEL_PINS(KICKOS_KERNEL_PIN);
    }
#undef KICKOS_KERNEL_PIN
    static_assert(kickos::refuses_exactly(f3_pin_kernel_owned, f3_pin_listed, PINMUX_PORT_MAX + 1u, 16u),
                  "arch_pinmux_set refuses other pins than the board's kernel pins");

    constexpr bool LED_LIT = KICKOS_BOARD_LED_ACTIVE_LOW == 0;

    void tim2_clock_init()
    {
        // Boot-order: nothing before arch_init may read the clock. A static ctor
        // (__init_array) calling ktime_now()/arch_clock_now() BusFaults here on
        // the ungated APB1 access.
        // The F3 has no APB1LPENR: TIM2 keeps counting in WFI by default.
        reg32(RCC_APB1ENR) |= APB1ENR_TIM2EN;
        reg32(mmap::TIM2_BASE + TIM_CR1) = 0;           // stop; upcount, defaults
        reg32(mmap::TIM2_BASE + TIM_PSC) = 0;           // no prescale: the timer kernel clock
        reg32(mmap::TIM2_BASE + TIM_ARR) = 0xFFFFFFFFu; // full 32-bit free-run
        reg32(mmap::TIM2_BASE + TIM_EGR) = TIM_EGR_UG;  // latch PSC/ARR into the shadows (sets UIF)
        reg32(chip::CLK_TIMER_SR) = ~TIM_SR_UIF;        // drop that UIF before arming the IRQ
        reg32(mmap::TIM2_BASE + TIM_DIER) = TIM_DIER_UIE; // wrap observer for the disarmed idle case
        reg32(mmap::TIM2_BASE + TIM_CR1) = TIM_CR1_CEN;   // enable
        // No arch_irq_clear_pending: a pend latched here (latch-and-coalesce) redelivers
        // one benign kickos_isr_timer tick on enable, which the tickless handler tolerates.
        arch_irq_unmask(chip::CLK_TIMER_IRQ); // NVIC enable in the maskable device band
    }

    void usart2_init()
    {
        reg32(RCC_AHBENR) |= (1u << (AHBENR_IOP_SHIFT + KICKOS_BOARD_CONSOLE_TX_PORT))
                             | (1u << (AHBENR_IOP_SHIFT + KICKOS_BOARD_CONSOLE_RX_PORT));
        reg32(RCC_APB1ENR) |= APB1ENR_USART2EN;
        console_pins_init();

        reg32(chip::USART_CR1) = 0; // BRR writable only while UE=0
        // USART2SEL resets to 00, so PCLK1 clocks USART2 and the BRR tracks the achieved
        // PCLK1: 32e6/115200 = 277.8 -> 278, -0.08%.
        reg32(chip::USART_BRR) = usart_brr(pclk1_hz, 115200u);
        reg32(chip::USART_CR1) = chip::CR1_UE | chip::CR1_TE | chip::CR1_RE; // TXEIE stays clear
    }
}

extern "C"
{

void arch_init(void)
{
    // FPU enabled earlier (Reset_Handler). Clock first (HSI/2 -> PLL -> 64 MHz),
    // then the console derives its BRR from the achieved PCLK1.
    if (kickos::stm32::clock_init())
    {
        SystemCoreClock = 64000000u;
        pclk1_hz = 32000000u;
    }
    tim2_clock_init(); // monotonic time base: the required arch_clock_now source
    // Anchor the clock ONCE, from the FINAL rate: TIM2 is on APB1 and, with HPRE=/1
    // and PPRE1 in {/1,/2}, the STM32 APB timer-clock doubler makes the timer kernel
    // clock equal HCLK == SystemCoreClock (retuning PPRE1 to /4+ would break that).
    kickos::stm32::clock_anchor(SystemCoreClock);
    usart2_init();
    kickos_armv7m_init();
}

// The STM32F302x8 has no MPU, unlike the F302xB/xC line: 0 is the seam's no-MPU
// granule (arch.h), not a tuning choice.
size_t arch_mpu_min_region(void)
{
    return 0u;
}

// ISR.TC, not ISR.TXE: TXE says the data register took the byte, TC says the shift register
// finished clocking it out. kickos_terminate stops the core right after this, so waiting on
// TXE would still lose the last character, as f302nucleo did, cutting
// its survivor line mid-word in every faultsurvive capture.
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while ((reg32(chip::USART_SR) & chip::USART_TC) == 0)
    {
        if (++spin > KICKOS_POLL_SPIN_MAX)
        {
            return; // bounded, as arch.h requires: a wedged UART drops the tail, never hangs
        }
    }
}

void arch_diag_led_init(void)
{
    constexpr uintptr_t moder = KICKOS_BOARD_LED_PORT_BASE + GPIO_MODER_OFF;
    reg32(RCC_AHBENR) |= (1u << (AHBENR_IOP_SHIFT + KICKOS_BOARD_LED_PORT));
    uint32_t m = reg32(moder);
    m &= ~(0x3u << (KICKOS_BOARD_LED_BIT * 2u));
    m |= (MODER_OUTPUT << (KICKOS_BOARD_LED_BIT * 2u));
    reg32(moder) = m;
}

void arch_diag_led_set(int on)
{
    constexpr uintptr_t bsrr = KICKOS_BOARD_LED_PORT_BASE + 0x18;
    if (on)
    {
        reg32(bsrr) = bsrr_word(KICKOS_BOARD_LED_BIT, LED_LIT);
    }
    else
    {
        reg32(bsrr) = bsrr_word(KICKOS_BOARD_LED_BIT, not LED_LIT);
    }
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET). func = bits[1:0] MODER field
// (verbatim) + bits[7:4] AF number. Validate range + kernel-owned BEFORE gating a
// clock or touching a register (a gate-then-fail path would leak an enabled clock).
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port > PINMUX_PORT_MAX or pin > 15u)
    {
        return -KOS_EINVAL;
    }
    if (f3_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    reg32(RCC_AHBENR) |= (1u << (AHBENR_IOP_SHIFT + port)); // gate this port's clock (idempotent)
    uintptr_t const base = mmap::GPIOA_BASE + port * mmap::GPIO_STRIDE;
    uint32_t const mode_shift = pin * 2u;
    uint32_t moder = reg32(base + GPIO_MODER_OFF);
    moder &= ~(0x3u << mode_shift);
    moder |= (func & 0x3u) << mode_shift;
    reg32(base + GPIO_MODER_OFF) = moder;
    uintptr_t afr = base + GPIO_AFRL_OFF;
    uint32_t af_shift = pin * 4u;
    if (pin >= 8u)
    {
        afr = base + GPIO_AFRH_OFF;
        af_shift = (pin - 8u) * 4u;
    }
    uint32_t afv = reg32(afr);
    afv &= ~(0xFu << af_shift);
    afv |= ((func >> 4) & 0xFu) << af_shift;
    reg32(afr) = afv;
    return 0;
}

void Reset_Handler(void)
{
    kickos_armv7m_enable_fpu();

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
