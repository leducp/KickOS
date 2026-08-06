// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 (STM32F411E-DISCO, Cortex-M4F) chip backend. Registers are clean-room
// from RM0383; hand-rolled, no vendor HAL/CMSIS, consistent with the arch layer.
//
// Clocking: HSE crystal (8 MHz on the
// F411E-DISCO) -> main PLL -> 84 MHz SYSCLK for an accurate, full-speed core (the
// HSI RC is too imprecise for reliable 115200 UART). clock_init() runs first in
// arch_init and bounded-polls every ready flag, so a dead/missing crystal degrades
// to the reset-default HSI 16 MHz instead of hanging (the BRR is recomputed from
// whichever APB1 clock we end up on). Console = USART2 on PA2(TX)/PA3(RX), AF7,
// polled TX. STM32 keeps peripheral clocks running in WFI, so no TX drain is needed
// (unlike the XMC). STM32 has no watchdog running at reset (unlike the K64F), so
// the reset path is FPU + C-runtime + clocks.

#include "regs.h" // arch/arm/common: kickos_armv7m_enable_fpu + core SCB regs
#include <kickos/chip_mmap.h>
#include "irq.h"
#include "regs/flash.h"
#include "regs/gpio.h"
#include "regs/rcc.h"
#include "regs/tim.h"
#include "regs/usart.h"

#include <kickos/arch/arch.h>
#include <kickos/arch/clk_anchor.h> // shared tickless-clock epoch anchor (B2)
#include <kickos/board_config.h> // per-board HSE freq + LED pin (Disco vs Black Pill)
#include <kickos/config/limits.h>
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h> // KOS_E* taxonomy (arch_pinmux_set)

#include <stdint.h>

namespace mmap = kickos::stm32f411::mmap;
namespace irq = kickos::stm32f411::irq;
namespace rcc = kickos::stm32f411::reg::rcc;
namespace flash = kickos::stm32f411::reg::flash;
namespace gpio = kickos::stm32f411::reg::gpio;
namespace tim = kickos::stm32f411::reg::tim;
namespace usart = kickos::stm32f411::reg::usart;

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

    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    uint32_t SystemCoreClock = 16000000u; // updated by clock_init(); HSI on fallback
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // Main PLL from HSE -> 84 MHz (RM lines 5232-5324). PLLM is chosen per board
    // to make VCO_in exactly 1 MHz regardless of the crystal (Disco 8 MHz -> PLLM
    // 8; Black Pill 25 MHz -> PLLM 25), so PLLN/PLLP/PLLQ are board-independent:
    //   VCO_in  = HSE / PLLM  = 1 MHz              (1..2 MHz, RM line 5316)
    //   VCO_out = VCO_in * PLLN = 1 MHz * 336 = 336 MHz (100..432 MHz, RM line 5293)
    //   SYSCLK  = VCO_out / PLLP = 336 / 4   = 84 MHz  (<=100 MHz, RM line 5280)
    //   PLL48   = VCO_out / PLLQ = 336 / 7   = 48 MHz  (USB/SDIO, RM line 5251)
    constexpr uint32_t PLLM = KICKOS_HSE_HZ / 1000000u; // board-derived, no fixed constant
    constexpr uint32_t PLLCFGR_VALUE =
        (rcc::PLLQ << rcc::PLLCFGR_PLLQ_SHIFT) | rcc::PLLCFGR_PLLSRC_HSE |
        rcc::PLLCFGR_PLLP_DIV4 | (rcc::PLLN << rcc::PLLCFGR_PLLN_SHIFT) |
        (PLLM << rcc::PLLCFGR_PLLM_SHIFT);

    constexpr uint32_t HSE_HZ = KICKOS_HSE_HZ;
    constexpr uint32_t SYSCLK_PLL_HZ = 84000000u;
    constexpr uint32_t PCLK1_PLL_HZ = 42000000u; // APB1 = 84/2

    // Bounded so a dead/missing crystal degrades to HSI instead of hanging boot
    // forever (a silent hang leaves no UART/LED sign of life). The cap is far
    // longer than any legitimate wait (HSE startup is well under 1 ms).
    constexpr uint32_t POLL_TIMEOUT = 1000000u;

    // APB1 clock the console runs on; set by clock_init(). Defaults to the HSI
    // fallback (SYSCLK=HCLK=PCLK1=16 MHz at reset) so the UART still works if the
    // crystal never comes up.
    uint32_t pclk1_hz = 16000000u;

    // GPIOA MODER (2b/pin) + AFRL (4b/pin, pins 0-7); USART2 pins PA2/PA3 = AF7.
    constexpr uintptr_t GPIOA_MODER = mmap::GPIOA_BASE + gpio::MODER;
    constexpr uintptr_t GPIOA_AFRL = mmap::GPIOA_BASE + gpio::AFRL;

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
        r32(flash::ACR) =
            flash::ACR_LATENCY_2WS | flash::ACR_PRFTEN | flash::ACR_ICEN | flash::ACR_DCEN;

        r32(rcc::CR) |= rcc::CR_HSEON;
        if (not wait_mask(rcc::CR, rcc::CR_HSERDY))
        {
            return; // no crystal: stay on HSI 16 MHz
        }

        // PLL config bits are writable only while PLL is off (RM lines 5250, 5279).
        r32(rcc::PLLCFGR) = PLLCFGR_VALUE;

        // Set bus prescalers before the fast clock is live so APB1<=42 / APB2<=84
        // are never briefly exceeded when SYSCLK switches to the PLL.
        r32(rcc::CFGR) = rcc::CFGR_HPRE_DIV1 | rcc::CFGR_PPRE1_DIV2 | rcc::CFGR_PPRE2_DIV1;

        r32(rcc::CR) |= rcc::CR_PLLON;
        if (not wait_mask(rcc::CR, rcc::CR_PLLRDY))
        {
            return; // PLL never locked: stay on HSI 16 MHz
        }

        r32(rcc::CFGR) = (r32(rcc::CFGR) & ~rcc::CFGR_SW_MASK) | rcc::CFGR_SW_PLL;
        if (not wait_mask(rcc::CFGR, rcc::CFGR_SWS_PLL)) // SWS reads back the active source
        {
            return; // switch did not take: HSI still drives SYSCLK
        }

        SystemCoreClock = SYSCLK_PLL_HZ;
        pclk1_hz = PCLK1_PLL_HZ;
    }

    // --- TIM2: the monotonic time base (RM0383 sec.13) --------------------------
    // The v7-M default clock is the DWT cycle counter (core debug power domain),
    // but that DWT intermittently returns aliased garbage on parts in this fleet,
    // which the software 32->64 wrap-extension turns into a phantom 2^32 jump that
    // strands every timed wait. TIM2 is a plain 32-bit general-purpose timer on
    // APB1 (not the debug domain): free-run it and use it as arch_clock_now.
    // arch_trace_now stays on raw DWT_CYCCNT, where a glitch costs one telemetry
    // sample. TIM2 does not collide with the one-shot tickless timer
    // (SysTick, core-generic) nor any driver (none use TIM2 on this port).

    // Software 64-bit extension of the 32-bit TIM2_CNT. Reads are RELIABLE (unlike
    // DWT): TIM2 wraps every 2^32/84e6 ~= 51 s. The wrap is folded either by a
    // thread read or, when the system is idle with the tickless timer disarmed, by
    // the TIM2 overflow ISR below, exactly once: whoever reads first advances
    // g_clk_last, so the other sees no backward step. Without that ISR a wrap
    // across a fully-quiescent >51 s idle would be lost (a slow DWT-style leap).
    volatile uint32_t g_clk_high = 0;
    volatile uint32_t g_clk_last = 0;

    // arch_clock_now epoch anchor (B2, shared: kickos/arch/clk_anchor.h). Sole writer
    // is init() in arch_init; this chip never retunes at runtime. A retune added later
    // must call reprice() at the rate edge; the read must stay pure.
    kickos::arch_clk_anchor g_clk;

    void tim2_clock_init()
    {
        // Boot-order: nothing before arch_init may read the clock. A static ctor
        // (__init_array) calling ktime_now()/arch_clock_now() BusFaults here on the
        // ungated APB1 access (it was a harmless DWT read before this override).
        r32(rcc::APB1ENR) |= rcc::APB1ENR_TIM2EN;
        // Keep TIM2 clocked in Sleep mode (WFI). TIM2LPEN resets to 1; clearing it
        // would freeze the clock the instant the idle thread executes WFI.
        r32(rcc::APB1LPENR) |= rcc::APB1ENR_TIM2EN;
        r32(tim::CR1) = 0;             // stop; upcount, defaults
        r32(tim::PSC) = 0;             // no prescale: count at the timer kernel clock
        r32(tim::ARR) = 0xFFFFFFFFu;   // full 32-bit free-run
        r32(tim::EGR) = tim::EGR_UG;   // latch PSC/ARR into the shadow regs (sets UIF)
        r32(tim::SR) = ~tim::SR_UIF;   // drop the UG-induced UIF before arming the IRQ
        r32(tim::DIER) = tim::DIER_UIE; // wrap observer for the disarmed-timer idle case
        r32(tim::CR1) = tim::CR1_CEN;  // enable
        // No arch_irq_clear_pending: a pend latched here (latch-and-coalesce) redelivers
        // one benign kickos_isr_timer tick on enable, which the tickless handler tolerates.
        arch_irq_unmask(irq::TIM2_IRQ); // NVIC enable in the maskable device band
    }

    // Wrap-catch must be atomic against a concurrent reader (thread + ISR), so the
    // extend runs under the crit section.
    uint64_t tim2_ticks()
    {
        arch_irq_state_t s = arch_irq_save();
        uint32_t cur = r32(tim::CNT);
        if (cur < g_clk_last)
        {
            g_clk_high++;
        }
        g_clk_last = cur;
        uint64_t hi = g_clk_high;
        arch_irq_restore(s);
        return (hi << 32) | cur;
    }

    void usart2_init()
    {
        r32(rcc::AHB1ENR) |= rcc::AHB1ENR_GPIOAEN;
        r32(rcc::APB1ENR) |= rcc::APB1ENR_USART2EN;

        // PA2/PA3 -> alternate-function mode (0b10), AF7 (USART2).
        uint32_t moder = r32(GPIOA_MODER);
        moder &= ~(0xFu << 4);                             // clear MODER2/MODER3 (bits 4..7)
        moder |= (gpio::MODER_AF << 4) | (gpio::MODER_AF << 6); // AF mode for PA2, PA3
        r32(GPIOA_MODER) = moder;
        uint32_t afrl = r32(GPIOA_AFRL);
        afrl &= ~(0xFFu << 8);                             // clear AFRL2/AFRL3 (bits 8..15)
        afrl |= (gpio::AF7 << 8) | (gpio::AF7 << 12);      // AF7 for PA2, PA3
        r32(GPIOA_AFRL) = afrl;

        r32(usart::CR1) = 0;         // disable while configuring (OVER8=0)
        r32(usart::BRR) = usart_brr(pclk1_hz, usart::BAUD_115200);
        r32(usart::CR1) = usart::CR1_UE | usart::CR1_TE | usart::CR1_RE; // TXEIE clear; ring primes it
    }

    // --- Buffered console TX backend (console_tx.h). The ring drains via the
    // USART2 TXE (TX-data-register-empty) interrupt, level-triggered: enabling
    // TXEIE while TXE=1 raises it immediately. slot_free/push touch one data
    // register; irq_enable/disable gate TXEIE at the peripheral. ---
    int f4_tx_slot_free(void) { return (r32(usart::SR) & usart::SR_TXE) != 0; }
    void f4_tx_push(uint8_t b) { r32(usart::DR) = b; }
    void f4_tx_irq_enable(void) { r32(usart::CR1) |= usart::CR1_TXEIE; }
    void f4_tx_irq_disable(void) { r32(usart::CR1) &= ~usart::CR1_TXEIE; }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const f4_console_backend = {
        f4_tx_slot_free, f4_tx_push, f4_tx_irq_enable, f4_tx_irq_disable};

}

extern "C"
{

void arch_init(void)
{
    // FPU is enabled earlier (Reset_Handler, before C++ ctors). Bring the core up
    // on the HSE crystal + PLL first, then configure the console at the resulting
    // APB1 clock (clock_init leaves us on HSI 16 MHz if the crystal is absent).
    clock_init();
    tim2_clock_init(); // monotonic time base (replaces the unreliable DWT clock)
    // Anchor the clock ONCE, from the FINAL rate: TIM2 is on APB1 and, with HPRE=/1
    // and PPRE1 in {/1,/2}, the STM32 APB timer-clock doubler makes the timer kernel
    // clock equal HCLK == SystemCoreClock (retuning PPRE1 to /4+ would break that).
    g_clk.init(SystemCoreClock);
    usart2_init();
    kickos_armv7m_init();
}

// Monotonic clock: free-running TIM2 ticks -> ns, the required per-chip source (the
// DWT-backed arch_clock_now (unreliable on this silicon). Pure epoch read: the anchor
// holds the rate, so no divide and no rate derivation happens here.
uint64_t arch_clock_now(void)
{
    return g_clk.ns_from(tim2_ticks());
}

// TIM2 overflow (update) ISR, vectored at NVIC 28 in startup.S. Its only job is to
// observe the 51 s wrap while the tickless timer is disarmed and no thread reads
// the clock; tim2_ticks folds it into g_clk_high (idempotent vs a concurrent
// thread read). Runs in the maskable band, so an IrqLock defers it harmlessly.
void kickos_tim2_clock_isr(void)
{
    r32(tim::SR) = ~tim::SR_UIF; // ack the update flag (rc_w0)
    tim2_ticks();
}

void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n); // buffered; the routing guard (console.cc) keeps this thread-only
}

void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((r32(usart::SR) & usart::SR_TXE) == 0)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(usart::DR) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = irq::USART2_IRQ;
    return &f4_console_backend;
}

// Kernel diagnostic LED: the pin/port/polarity are board facts (KICKOS_LED_*,
// from board_config.h) so one stm32f411 backend drives the Disco's PD12
// (active-high) and the Black Pill's PC13 (active-low) unchanged.
void arch_diag_led_init(void)
{
    r32(rcc::AHB1ENR) |= (1u << KICKOS_LED_RCC_AHB1_BIT);
    uint32_t m = r32(KICKOS_LED_GPIO + gpio::MODER);
    m &= ~(0x3u << (KICKOS_LED_PIN * 2));
    m |= (gpio::MODER_OUTPUT << (KICKOS_LED_PIN * 2)); // general-purpose output
    r32(KICKOS_LED_GPIO + gpio::MODER) = m;
}

void arch_diag_led_set(int on)
{
    constexpr uintptr_t bsrr = KICKOS_LED_GPIO + gpio::BSRR; // [15:0]=set, [31:16]=reset
    bool high = (on != 0);
#if KICKOS_LED_ACTIVE_LOW
    high = not high; // lit when driven low
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

// Board LED port index, derived from the LED's GPIO base at compile time (Disco PD12
// -> port 3; Black Pill PC13 -> port 2). Refused so a board map cannot steal the LED.
constexpr uint32_t LED_PORT_INDEX = (KICKOS_LED_GPIO - mmap::GPIOA_BASE) / mmap::GPIO_STRIDE;

// Kernel-owned pins arch_pinmux_set refuses so a board map cannot dark the console or
// steal the diag LED. PA2/PA3 = USART2 TX/RX; the LED port/pin is board-derived above.
static bool f411_pin_kernel_owned(uint32_t port, uint32_t pin)
{
    if (port == 0u and (pin == 2u or pin == 3u))
    {
        return true;
    }
    if (port == LED_PORT_INDEX and pin == KICKOS_LED_PIN)
    {
        return true;
    }
    return false;
}

// Preset an output pin high as part of the same call. Refused with -KOS_EINVAL when the
// MODER field is not output, so a stray bit cannot pass for a working request.
constexpr uint32_t PINMUX_OUT_HIGH = 1u << 8;

// One-shot pin-function config (KOS_SYS_PINMUX_SET). func layout, three fields:
//   [1:0] MODER field written verbatim (00=in, 01=out, 10=AF, 11=analog)
//   [7:4] AF number (AFRL for pin<8, AFRH for pin>=8)
//   [8]   PINMUX_OUT_HIGH, output-only: drive the pin high (see the ordering note below)
// Gates the port's AHB1ENR bit (=port) first (an unclocked GPIO register access faults).
// PUPDR/OSPEEDR stay at reset, so slew and pulls are not reachable through this seam.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port > 7u or pin > 15u)
    {
        return -KOS_EINVAL;
    }
    uint32_t const mode = func & 0x3u;
    bool const preset_high = (func & PINMUX_OUT_HIGH) != 0u;
    if (preset_high and mode != gpio::MODER_OUTPUT)
    {
        return -KOS_EINVAL;
    }
    if (f411_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    r32(rcc::AHB1ENR) |= (1u << port); // gate this port's clock (idempotent)
    // The gate needs an intervening bus transaction before the BSRR store or that store
    // is dropped and the pin takes the ODR reset level (low) when MODER switches
    // (mechanism: pit_clock_init in arch/arm/chip/mk64f/chip_mk64f.cc).
    uint32_t const gate = r32(rcc::AHB1ENR);
    __asm volatile("" ::"r"(gate) : "memory");
    uintptr_t const base = mmap::GPIOA_BASE + port * mmap::GPIO_STRIDE;
    // Level BEFORE MODER: a BSRR set on a still-input pin is inert and the MODER switch
    // then drives high directly. Reversed, the pin asserts the ODR reset level (low) first.
    if (preset_high)
    {
        r32(base + gpio::BSRR) = 1u << pin; // atomic set, no ODR readback
    }
    uint32_t moder = r32(base + gpio::MODER);
    moder &= ~(0x3u << (pin * 2u));
    moder |= mode << (pin * 2u);
    r32(base + gpio::MODER) = moder;

    uint32_t const af = (func >> 4u) & 0xFu;
    uintptr_t afr = base + gpio::AFRL;
    uint32_t shift = pin * 4u;
    if (pin >= 8u)
    {
        afr = base + gpio::AFRH;
        shift = (pin - 8u) * 4u;
    }
    uint32_t v = r32(afr);
    v &= ~(0xFu << shift);
    v |= af << shift;
    r32(afr) = v;
    return 0;
}

// Per-block enable table (KOS_SYS_PERIPH_ENABLE), keyed on the EXACT block base.
// Clock gate only: no privilege-classification register exists for this bus in this tree.
// GPIO port clocks are absent here; arch_pinmux_set owns those AHB1ENR bits.
int arch_periph_enable(uintptr_t base)
{
    if (base == mmap::SPI1_BASE)
    {
        r32(rcc::APB2ENR) |= rcc::APB2ENR_SPI1EN; // idempotent
        return 0;
    }
    return -KOS_EINVAL;
}

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (RM0383). Owns-for-life: the TIM2 monotonic time base and the
// RCC clock/reset/gate block. TIM2 and RCC bases are the constants above; sizes are
// one 1 KB APB slot each per the RM peripheral map.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {mmap::TIM2_BASE, 0x400u},  // TIM2: the monotonic time base (RM sec.13)
        {mmap::RCC_BASE, 0x400u},   // RCC: clock/PLL + peripheral reset/gate (RM sec.6)
    };
    size_t n = sizeof(blocks) / sizeof(blocks[0]);
    if (n > max)
    {
        n = max;
    }
    for (size_t i = 0; i < n; i++)
    {
        out[i] = blocks[i];
    }
    return n;
}
#endif

// STM32F411 is a Cortex-M4 with the bit-band peripheral/SRAM alias.
int arch_bitband_present(void)
{
    return 1;
}

void Reset_Handler(void)
{
    kickos_armv7m_enable_fpu(); // before any code that a hard-float ABI might emit FP into

    kickos_ranges_init(); // init .data + the pow2 app-data block; zero .bss + app-bss
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
