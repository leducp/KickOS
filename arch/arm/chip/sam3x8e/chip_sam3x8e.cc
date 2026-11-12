// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Atmel/Microchip AT91SAM3X8E (Arduino Due, Cortex-M3) chip backend (datasheet Atmel-11057).

#include <kickos/arch/arch.h>
#include <kickos/arch/console_poll.h>
#include "crt_tail.h"
#include "pin_guard.h"
#include <kickos/config/limits.h>
#include <kickos/arch/clk_anchor.h>
#include <kickos/arch/wrap_extend.h>
#include <kickos/console_tx.h>
#include <kickos/io/mmio.h>
#include <kickos/sys/abi.h>

#include <kickos/chip_mmap.h>
#include "board_pins.h"
#include "irq.h"
#include <chip_layout.h>
#include "regs.h"
#include "uart_baud.h"
#include "clock_bringup.h"

#include <stdint.h>

extern "C"
{
    void kickos_armv7m_ccr_init(void);
    void kickos_armv7m_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

    // PMC_MCKR comes out of reset selecting MAINCK undivided (sec.28.15.11, reset 0x1) and
    // MAINCK is the fast RC, so MCK is this until clock_init moves one of the two terms.
    uint32_t SystemCoreClock = kickos::sam3x8e::clocktree::MAINCK_RC_HZ;
}

namespace
{
    namespace mmap = kickos::sam3x8e::mmap;
    namespace irq = kickos::sam3x8e::irq;
    static_assert(irq::TC0_IRQ == KICKOS_LAYOUT_LINE_TC0_IRQ,
                  "startup.S vectors the clock-wrap ISR at KICKOS_LAYOUT_LINE_TC0_IRQ");

    constexpr uintptr_t WDT_MR = mmap::WDT_BASE + 0x04; // write-once
    constexpr uint32_t WDT_MR_WDDIS = 1u << 15;

    constexpr uintptr_t PMC_PCER0 = mmap::PMC_BASE + 0x10;
    constexpr uint32_t PID_UART = 1u << 8;

    static_assert(KICKOS_BOARD_CONSOLE_BASE == mmap::UART_BASE,
                  "the board's console is not the UART this backend drives");
    static_assert(KICKOS_BOARD_CONSOLE_TX_SELECT == 0 and KICKOS_BOARD_CONSOLE_RX_SELECT == 0,
                  "the console pins stay on peripheral A, the ABSR reset value this backend leaves");

    // PMC_PCER0 clock bit = 11+port. func selects the routing:
    //   0x00 = GPIO output (PIO_PER + PIO_OER), 0x01 = GPIO input (PIO_PER + PIO_ODR),
    //   0x10 = peripheral A (ABSR bit CLEAR, then PIO_PDR),
    //   0x11 = peripheral B (ABSR bit SET,   then PIO_PDR).
    // The ABSR write MUST precede PDR, which hands the pin to whichever peripheral ABSR selects.
    // The OER/ODR write is MANDATORY: PER alone leaves a dead output. Pull-ups are on at reset
    // and left alone.
    constexpr uintptr_t PIO_PER_OFF = 0x00;
    constexpr uintptr_t PIO_PDR_OFF = 0x04;
    constexpr uintptr_t PIO_OER_OFF = 0x10;
    constexpr uintptr_t PIO_ODR_OFF = 0x14;
    constexpr uintptr_t PIO_SODR_OFF = 0x30;
    constexpr uintptr_t PIO_CODR_OFF = 0x34;
    constexpr uintptr_t PIO_ABSR_OFF = 0x70;
    constexpr uint32_t PMC_PID_PIO_SHIFT = 11u;
    constexpr uint32_t PINMUX_PORT_MAX = 3u; // PIOA..PIOD
    constexpr uint32_t PINMUX_FUNC_GPIO_OUT = 0x00u;
    constexpr uint32_t PINMUX_FUNC_GPIO_IN = 0x01u;
    constexpr uint32_t PINMUX_FUNC_PERIPH_A = 0x10u;
    constexpr uint32_t PINMUX_FUNC_PERIPH_B = 0x11u;

    constexpr uintptr_t led_out(bool level)
    {
        if (level)
        {
            return KICKOS_BOARD_LED_PORT_BASE + PIO_SODR_OFF;
        }
        return KICKOS_BOARD_LED_PORT_BASE + PIO_CODR_OFF;
    }

    constexpr bool LED_LIT = KICKOS_BOARD_LED_ACTIVE_LOW == 0;

    void console_pins_init()
    {
        constexpr uint32_t tx = 1u << KICKOS_BOARD_CONSOLE_TX_BIT;
        constexpr uint32_t rx = 1u << KICKOS_BOARD_CONSOLE_RX_BIT;
        if constexpr (KICKOS_BOARD_CONSOLE_TX_PORT_BASE == KICKOS_BOARD_CONSOLE_RX_PORT_BASE)
        {
            r32(KICKOS_BOARD_CONSOLE_TX_PORT_BASE + PIO_PDR_OFF) = tx | rx;
        }
        else
        {
            r32(KICKOS_BOARD_CONSOLE_TX_PORT_BASE + PIO_PDR_OFF) = tx;
            r32(KICKOS_BOARD_CONSOLE_RX_PORT_BASE + PIO_PDR_OFF) = rx;
        }
    }

    constexpr uintptr_t UART_CR = mmap::UART_BASE + 0x00;
    constexpr uintptr_t UART_MR = mmap::UART_BASE + 0x04;
    constexpr uintptr_t UART_IER = mmap::UART_BASE + 0x08; // interrupt enable (write 1 to set)
    constexpr uintptr_t UART_IDR = mmap::UART_BASE + 0x0C; // interrupt disable (write 1 to clear)
    constexpr uintptr_t UART_SR = mmap::UART_BASE + 0x14;
    constexpr uintptr_t UART_THR = mmap::UART_BASE + 0x1C;
    constexpr uintptr_t UART_BRGR = mmap::UART_BASE + 0x20;
    constexpr uint32_t CR_RSTRX_RSTTX = (1u << 2) | (1u << 3);
    constexpr uint32_t CR_RXEN_TXEN = (1u << 4) | (1u << 6);
    constexpr uint32_t MR_NO_PARITY = 4u << 9; // PAR=100 (none), CHMODE=normal
    constexpr uint32_t SR_TXRDY = 1u << 1;
    constexpr uint32_t IER_TXRDY = 1u << 1; // TXRDY bit in IER/IDR/IMR (same position as SR)
    constexpr uint32_t CONSOLE_BAUD = 115200u;

    // The UART has no fractional divisor and a fixed 16x oversample: 84 MHz gives CD 46 = 114130
    // baud (-0.93%), while NONE of the degrade rates (12, 6, 4 MHz) reaches 115200 within what
    // 8N1 tolerates, so the console is off on those.

    // --- TC0 channel 0: the monotonic time base --------------------------------------
    // Not DWT_CYCCNT: it intermittently reads aliased garbage here, which the 32->64 wrap
    // extension turns into a phantom 2^32 jump that strands every timed wait. TC0 ch0 free-runs
    // in capture mode (RC never resets it) off TIMER_CLOCK1 = MCK/2.
    constexpr uintptr_t TC0_CCR0 = mmap::TC0_BASE + 0x00;
    constexpr uintptr_t TC0_CMR0 = mmap::TC0_BASE + 0x04;
    constexpr uintptr_t TC0_CV0 = mmap::TC0_BASE + 0x10;
    constexpr uintptr_t TC0_SR0 = mmap::TC0_BASE + 0x20;  // status (read clears flags)
    constexpr uintptr_t TC0_IER0 = mmap::TC0_BASE + 0x24; // interrupt enable (write-1-set)
    constexpr uint32_t TC_CMR_TCCLKS_MCK2 = 0x0u << 0; // TIMER_CLOCK1 = MCK/2
    constexpr uint32_t TC_CCR_CLKEN = 1u << 0;
    constexpr uint32_t TC_CCR_SWTRG = 1u << 2;
    constexpr uint32_t TC_SR_COVFS = 1u << 0;
    constexpr uint32_t PID_TC0 = 1u << 27;    // TC0 channel 0 = peripheral ID 27

    // Reading TC_SR clears COVFS, so the latch a reader may look at is the line's NVIC pend
    // bit. That pend clears as the ISR is entered, before take(): sound only while no reader
    // can preempt this ISR, which every device line sharing PRIO_DEVICE guarantees.
    struct tc0_wrap_ops
    {
        static uint32_t count() { return r32(TC0_CV0); }
        static bool pending()
        {
            constexpr unsigned line = static_cast<unsigned>(irq::TC0_IRQ);
            return (r32(kickos::arm::NVIC_ISPR0 + (line >> 5) * 4) & (1u << (line & 31))) != 0;
        }
        static bool take() { return (r32(TC0_SR0) & TC_SR_COVFS) != 0; }
    };
    kickos::arch_wrap_extend<tc0_wrap_ops> g_clk_wrap;

    // Sole writer is init() in arch_init; a runtime retune must call reprice() at the rate edge.
    kickos::arch_clk_anchor g_clk;

    void tc_clock_init()
    {
        // Nothing before arch_init may read the clock: a static ctor calling arch_clock_now()
        // BusFaults on the ungated TC access.
        // TC0 keeps counting in WFI only in Sleep mode (PMC_FSMR.LPM=0, the default): Wait mode
        // stops MCK, freezing TC0 AND SysTick.
        r32(PMC_PCER0) = PID_TC0;
        r32(TC0_CMR0) = TC_CMR_TCCLKS_MCK2;
        r32(TC0_CCR0) = TC_CCR_CLKEN | TC_CCR_SWTRG;
        uint32_t drop = r32(TC0_SR0);             // read-to-clear
        (void)drop;                               // ((void)r32) would elide the access
        r32(TC0_IER0) = TC_SR_COVFS;
        arch_irq_unmask(irq::TC0_IRQ);
    }

    void uart_init()
    {
        r32(PMC_PCER0) = PID_UART | (1u << (PMC_PID_PIO_SHIFT + KICKOS_BOARD_CONSOLE_TX_PORT))
                         | (1u << (PMC_PID_PIO_SHIFT + KICKOS_BOARD_CONSOLE_RX_PORT));
        console_pins_init();
        r32(UART_CR) = CR_RSTRX_RSTTX;
        r32(UART_MR) = MR_NO_PARITY;
        r32(UART_BRGR) = kickos::sam3x8e::uart_brgr_cd(SystemCoreClock, CONSOLE_BAUD);
        r32(UART_IDR) = 0xFFFFFFFFu; // all UART interrupt sources off; the ring arms TXRDY
        r32(UART_CR) = CR_RXEN_TXEN;
    }

    // TXRDY is level-triggered: writing IER.TXRDY while SR.TXRDY=1 raises the IRQ at once.
    int sam_tx_slot_free(void) { return (r32(UART_SR) & SR_TXRDY) != 0; }
    void sam_tx_push(uint8_t b) { r32(UART_THR) = b; }
    void sam_tx_irq_enable(void) { r32(UART_IER) = IER_TXRDY; }
    void sam_tx_irq_disable(void) { r32(UART_IDR) = IER_TXRDY; }
    console_tx_backend const sam_console_backend = {
        sam_tx_slot_free, sam_tx_push, sam_tx_irq_enable, sam_tx_irq_disable,
        &kickos_console_tx_storage};

}

extern "C"
{

void arch_init(void)
{
    SystemCoreClock = kickos::sam3x8e::clocktree::bring_up();
    tc_clock_init();
    // TC0 ch0 ticks at MCK/2.
    g_clk.init(SystemCoreClock / 2u);
    uart_init();
    kickos_armv7m_init();
}

// Must stay a pure read: deriving the rate here would reprice all elapsed history.
uint64_t arch_clock_now(void)
{
    return g_clk.ns_from(g_clk_wrap.read());
}

void kickos_tc0_clock_isr(void)
{
    g_clk_wrap.on_overflow();
}

bool arch_console_write_sync(char const* buf, size_t n)
{
    return kickos::console_poll::write<sam_tx_slot_free, sam_tx_push>(buf, n);
}

console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
    *irq_line = irq::UART_IRQ;
    return &sam_console_backend;
}

void arch_diag_led_init(void)
{
    r32(PMC_PCER0) = 1u << (PMC_PID_PIO_SHIFT + KICKOS_BOARD_LED_PORT);
    r32(KICKOS_BOARD_LED_PORT_BASE + PIO_PER_OFF) = 1u << KICKOS_BOARD_LED_BIT;
    r32(KICKOS_BOARD_LED_PORT_BASE + PIO_OER_OFF) = 1u << KICKOS_BOARD_LED_BIT;
}

void arch_diag_led_set(int on)
{
    if (on)
    {
        r32(led_out(LED_LIT)) = 1u << KICKOS_BOARD_LED_BIT;
    }
    else
    {
        r32(led_out(not LED_LIT)) = 1u << KICKOS_BOARD_LED_BIT;
    }
}

// Validate BEFORE gating a clock: a gate-then-fail path would leak an enabled clock.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port > PINMUX_PORT_MAX or pin > 31u)
    {
        return -KOS_EINVAL;
    }
    if (func != PINMUX_FUNC_GPIO_OUT and func != PINMUX_FUNC_GPIO_IN and
        func != PINMUX_FUNC_PERIPH_A and func != PINMUX_FUNC_PERIPH_B)
    {
        return -KOS_EINVAL;
    }
    if (kickos::board_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    r32(PMC_PCER0) = 1u << (PMC_PID_PIO_SHIFT + port); // write-1-to-set
    uintptr_t const base = mmap::PIOA_BASE + port * mmap::PIO_STRIDE;
    uint32_t const mask = 1u << pin;
    if (func == PINMUX_FUNC_GPIO_OUT)
    {
        r32(base + PIO_PER_OFF) = mask;
        r32(base + PIO_OER_OFF) = mask;
    }
    else if (func == PINMUX_FUNC_GPIO_IN)
    {
        r32(base + PIO_PER_OFF) = mask;
        r32(base + PIO_ODR_OFF) = mask;
    }
    else
    {
        uint32_t absr = r32(base + PIO_ABSR_OFF);
        if (func == PINMUX_FUNC_PERIPH_B)
        {
            absr |= mask;
        }
        else
        {
            absr &= ~mask;
        }
        r32(base + PIO_ABSR_OFF) = absr;
        r32(base + PIO_PDR_OFF) = mask;
    }
    return 0;
}

void Reset_Handler(void)
{
    // FIRST: the watchdog is enabled at reset and WDT_MR is write-once.
    r32(WDT_MR) = WDT_MR_WDDIS;
    kickos_armv7m_ccr_init();
    // The vector table lives at 0x0008_0000; reset fetched it through the 0x0 boot alias.
    r32(kickos::arm::SCB_VTOR) = mmap::FLASH_BASE;

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
    kickos_crt_images_data();
    kickos_crt_tail();
}

}
