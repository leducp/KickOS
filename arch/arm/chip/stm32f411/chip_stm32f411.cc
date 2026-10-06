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
// whichever APB1 clock we end up on). Console = USART2, on the pins the board file names,
// buffered TX drained by the USART2 TXE interrupt; arch_console_write_sync is the
// polled writer for panic/fault. STM32 keeps peripheral clocks running in WFI, so the
// TXE drain continues while the core sleeps (unlike the XMC). STM32 has no watchdog
// running at reset (unlike the K64F), so the reset path is FPU + C-runtime + clocks.

#include "regs.h" // arch/arm/common: kickos_armv7m_enable_fpu + core SCB regs
#include <kickos/chip_mmap.h>
#include "irq.h"
#include "regs/flash.h"
#include "regs/gpio.h"
#include "regs/rcc.h"
#include "regs/tim.h"
#include "regs/usart.h"
#include "board_pins.h"
#include "stm32_gpio.h"

#include <kickos/arch/arch.h>
#include <kickos/arch/pin_guard.h>
#include <kickos/arch/clk_anchor.h> // shared tickless-clock epoch anchor (B2)
#include <kickos/board_config.h>
// Required: a board on this chip that ships no boards/<board>/include/kickos/board_wiring.h
// fails here rather than compiling against another board's crystal.
#include <kickos/board_wiring.h>
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
namespace stm32 = kickos::stm32;

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

    static_assert(KICKOS_BOARD_CONSOLE_BASE == mmap::USART2_BASE,
                  "the board's console is not the USART this backend drives");

    void console_pins_init()
    {
        constexpr uintptr_t tx = KICKOS_BOARD_CONSOLE_TX_PORT_BASE;
        constexpr uintptr_t rx = KICKOS_BOARD_CONSOLE_RX_PORT_BASE;
        constexpr uint32_t tx_pin = KICKOS_BOARD_CONSOLE_TX_BIT;
        constexpr uint32_t rx_pin = KICKOS_BOARD_CONSOLE_RX_BIT;
        constexpr uintptr_t tx_afr = stm32::nibble_reg(tx + gpio::AFRL, tx_pin);
        constexpr uintptr_t rx_afr = stm32::nibble_reg(rx + gpio::AFRL, rx_pin);
        constexpr uint32_t tx_af = stm32::nibble_shift(tx_pin);
        constexpr uint32_t rx_af = stm32::nibble_shift(rx_pin);
        if constexpr (tx == rx)
        {
            stm32::rmw(tx + gpio::MODER, (0x3u << (tx_pin * 2u)) | (0x3u << (rx_pin * 2u)),
                       (gpio::MODER_AF << (tx_pin * 2u)) | (gpio::MODER_AF << (rx_pin * 2u)));
        }
        else
        {
            stm32::rmw(tx + gpio::MODER, 0x3u << (tx_pin * 2u), gpio::MODER_AF << (tx_pin * 2u));
            stm32::rmw(rx + gpio::MODER, 0x3u << (rx_pin * 2u), gpio::MODER_AF << (rx_pin * 2u));
        }
        if constexpr (tx_afr == rx_afr)
        {
            stm32::rmw(tx_afr, (0xFu << tx_af) | (0xFu << rx_af),
                       (uint32_t{KICKOS_BOARD_CONSOLE_TX_SELECT} << tx_af) | (uint32_t{KICKOS_BOARD_CONSOLE_RX_SELECT} << rx_af));
        }
        else
        {
            stm32::rmw(tx_afr, 0xFu << tx_af, uint32_t{KICKOS_BOARD_CONSOLE_TX_SELECT} << tx_af);
            stm32::rmw(rx_afr, 0xFu << rx_af, uint32_t{KICKOS_BOARD_CONSOLE_RX_SELECT} << rx_af);
        }
    }

    constexpr bool LED_LIT = KICKOS_BOARD_LED_ACTIVE_LOW == 0;

#define KICKOS_RESERVED_RUN(port_base, first, last) \
    or (mmap::GPIOA_BASE + port * mmap::GPIO_STRIDE == (port_base) and pin >= (first) and pin <= (last))
    constexpr bool f411_pin_kernel_owned(uint32_t port, uint32_t pin)
    {
        if (port == KICKOS_BOARD_CONSOLE_TX_PORT and pin == KICKOS_BOARD_CONSOLE_TX_BIT)
        {
            return true;
        }
        if (port == KICKOS_BOARD_CONSOLE_RX_PORT and pin == KICKOS_BOARD_CONSOLE_RX_BIT)
        {
            return true;
        }
        if (port == KICKOS_BOARD_LED_PORT and pin == KICKOS_BOARD_LED_BIT)
        {
            return true;
        }
        return false KICKOS_BOARD_RESERVED_RUNS(KICKOS_RESERVED_RUN);
    }
#undef KICKOS_RESERVED_RUN

#define KICKOS_KERNEL_PIN(port_base, bit) or (mmap::GPIOA_BASE + port * mmap::GPIO_STRIDE == (port_base) and pin == (bit))
    constexpr bool f411_pin_listed(uint32_t port, uint32_t pin)
    {
        return false KICKOS_BOARD_KERNEL_PINS(KICKOS_KERNEL_PIN);
    }
#undef KICKOS_KERNEL_PIN
    static_assert(kickos::refuses_exactly(f411_pin_kernel_owned, f411_pin_listed, 8u, 16u),
                  "arch_pinmux_set refuses other pins than the board's kernel pins");

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
    // arch_clock_now is a REQUIRED chip contract: the armv7m layer ships no clock
    // fallback. The obvious source, the DWT cycle counter, sits in the core debug
    // power domain and intermittently returns aliased garbage on parts in this fleet,
    // which the software 32->64 wrap-extension turns into a phantom 2^32 jump that
    // strands every timed wait. TIM2 is a plain 32-bit general-purpose timer on
    // APB1: free-run it and use it as arch_clock_now.
    // arch_trace_now stays on raw DWT_CYCCNT, where a glitch costs one telemetry
    // sample. TIM2 does not collide with the one-shot tickless timer
    // (SysTick, core-generic) nor any driver (none use TIM2 on this port).

    // Software 64-bit extension of the 32-bit TIM2_CNT. Reads are RELIABLE (unlike
    // DWT): TIM2 wraps every 2^32/84e6 ~= 51 s. The wrap is folded either by a
    // thread read or, when the system is idle with the tickless timer disarmed, by
    // the TIM2 overflow ISR below, exactly once: whoever reads first advances
    // g_clk_last, so the other sees no backward step. Without that ISR a wrap
    // across a fully-quiescent >51 s idle would be lost (a slow DWT-style leap).
    // The two words are ONE value: the IrqLock in tim2_ticks is what keeps them
    // coherent against the TIM2 overflow ISR, not the atomicity of either word.
    uint32_t g_clk_high = 0;
    uint32_t g_clk_last = 0;

    // arch_clock_now epoch anchor (B2, shared: kickos/arch/clk_anchor.h). Sole writer
    // is init() in arch_init; this chip never retunes at runtime. A retune added later
    // must call reprice() at the rate edge; the read must stay pure.
    kickos::arch_clk_anchor g_clk;

    void tim2_clock_init()
    {
        // Boot-order: nothing before arch_init may read the clock. A static ctor
        // (__init_array) calling ktime_now()/arch_clock_now() BusFaults here on
        // the ungated APB1 access.
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
            ++g_clk_high;
        }
        g_clk_last = cur;
        uint64_t hi = g_clk_high;
        arch_irq_restore(s);
        return (hi << 32) | cur;
    }

    void usart2_init()
    {
        r32(rcc::AHB1ENR) |= (1u << KICKOS_BOARD_CONSOLE_TX_PORT) | (1u << KICKOS_BOARD_CONSOLE_RX_PORT);
        r32(rcc::APB1ENR) |= rcc::APB1ENR_USART2EN;
        console_pins_init();

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
    char console_tx_buf[KICKOS_CONSOLE_TX_SIZE];
    console_tx_backend const f4_console_backend = {
        f4_tx_slot_free, f4_tx_push, f4_tx_irq_enable, f4_tx_irq_disable};

    // The window arch_console_reclaim rewrites. ONE constant: a reclaim reaching outside
    // the window it reports would rewrite registers whose holder was never checked. The
    // USART register file ends at 0x1B (RM0383 sec.19.6.8 Table 88), so this covers every
    // register that exists on the channel, and it need not equal the driver's grant:
    // dev_window_free tests OVERLAP, so any holder able to reach a register below also
    // overlaps this.
    constexpr uintptr_t CONSOLE_WIN_BASE = mmap::USART2_BASE;
    constexpr size_t CONSOLE_WIN_SIZE = usart::BLOCK_SIZE;

    // Every register the reclaim body writes must lie inside that window; adding a store
    // outside it fails to build rather than silently widening the reclaim's reach.
    static_assert(usart::CR1 >= CONSOLE_WIN_BASE
                  and usart::CR1 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and usart::CR2 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and usart::CR3 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and usart::GTPR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and usart::BRR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE,
                  "arch_console_reclaim writes outside the window it reports");

}

extern "C"
{

void arch_init(void)
{
    // FPU is enabled earlier (Reset_Handler, before C++ ctors). Bring the core up
    // on the HSE crystal + PLL first, then configure the console at the resulting
    // APB1 clock (clock_init leaves us on HSI 16 MHz if the crystal is absent).
    clock_init();
    tim2_clock_init(); // monotonic time base: the required arch_clock_now source
    // Anchor the clock ONCE, from the FINAL rate: TIM2 is on APB1 and, with HPRE=/1
    // and PPRE1 in {/1,/2}, the STM32 APB timer-clock doubler makes the timer kernel
    // clock equal HCLK == SystemCoreClock (retuning PPRE1 to /4+ would break that).
    g_clk.init(SystemCoreClock);
    usart2_init();
    kickos_armv7m_init();
}

// Monotonic clock: free-running TIM2 ticks -> ns, the required per-chip arch_clock_now.
// Pure epoch read: the anchor holds the rate, so no divide and no rate derivation
// happens here.
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

int arch_console_write(char const* buf, size_t n)
{
    return console_tx_insert_line(buf, n, KICKOS_CONSOLE_CRLF);
}

bool arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((r32(usart::SR) & usart::SR_TXE) == 0)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                return false; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(usart::DR) = static_cast<uint8_t>(buf[i]);
    }
    return true;
}

// SR.TC, not SR.TXE: TXE says DR handed the byte to the shift register, TC says the shift
// register finished clocking it out, stop bit included (RM0383 sec.19.3.2). kickos_terminate
// stops the core right after this, so waiting on TXE would still lose the last character.
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while ((r32(usart::SR) & usart::SR_TC) == 0)
    {
        if (++spin > KICKOS_POLL_SPIN_MAX)
        {
            return; // bounded, as arch.h requires: a wedged UART drops the tail, never hangs
        }
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = KICKOS_CONSOLE_TX_SIZE;
    *irq_line = irq::USART2_IRQ;
    return &f4_console_backend;
}

// USART2 (RM0383 sec.19, APB1). On f4uartirq the IRQ thread holds this grant, not the
// service thread whose death notes the console dead.
void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Panic-path reclaim (console.cc D6): force USART2 back to a known polled-ready 8N1 TX
// channel after a userspace driver may have garbled EVERY writable register in its granted
// window. Runs with IRQs masked, privileged; MUST be idempotent + re-entrant, so it is
// straight-line ABSOLUTE stores only, NO read-modify-write: an RMW on a garbled value is not
// safe to repeat from a nested-fault re-entry.
//
// Reclaim depth = what usart2_init sets (BRR, CR1) plus the registers init leaves at reset
// default that a driver can set to cause SILENT LOSS, each cleared to 0 below with the
// failure it prevents. The clock gate (RCC_APB1ENR.USART2EN) and the pin mux sit outside the
// window: RCC is an arch_reserved_blocks entry, and arch_pinmux_set refuses the console pins
// as kernel-owned.
void arch_console_reclaim(void)
{
    // FIRST, and the order is load-bearing: CR2's CPOL, CPHA and LBCL must not be written
    // while the transmitter is enabled (RM0383 sec.19.6.5), and this is what clears TE.
    r32(usart::CR1) = 0; // UE=0: channel off, every interrupt source disarmed
    // CTSE postpones every byte until an unwired CTS is asserted, which is the true
    // silent-loss case here; IREN modulates the TX pin; HDSEL ties TX to RX; SCEN/NACK add
    // smartcard framing; DMAT/DMAR hand the data register to a DMA request instead of ours.
    r32(usart::CR3) = 0;
    r32(usart::CR2) = 0;  // STOP=00 (1 stop bit), no LIN, no synchronous clock on the pin
    r32(usart::GTPR) = 0; // guard time + smartcard/IrDA prescaler

    // Re-derive the divisor from the live APB1 clock, as usart2_init does: 42 MHz on the
    // PLL, or the 16 MHz HSI fallback when the crystal never came up.
    r32(usart::BRR) = usart_brr(pclk1_hz, usart::BAUD_115200);

    // 8N1, OVER8=0, TXEIE clear: the polled writer in arch_console_write_sync is what has to
    // work after this, and console_tx re-arms TXEIE itself when the ring is used again.
    r32(usart::CR1) = usart::CR1_UE | usart::CR1_TE | usart::CR1_RE;
}

void arch_diag_led_init(void)
{
    r32(rcc::AHB1ENR) |= (1u << KICKOS_BOARD_LED_PORT);
    uint32_t m = r32(KICKOS_BOARD_LED_PORT_BASE + gpio::MODER);
    m &= ~(0x3u << (KICKOS_BOARD_LED_BIT * 2));
    m |= (gpio::MODER_OUTPUT << (KICKOS_BOARD_LED_BIT * 2));
    r32(KICKOS_BOARD_LED_PORT_BASE + gpio::MODER) = m;
}

void arch_diag_led_set(int on)
{
    constexpr uintptr_t bsrr = KICKOS_BOARD_LED_PORT_BASE + gpio::BSRR;
    bool const high = (on != 0) == LED_LIT;
    if (high)
    {
        r32(bsrr) = stm32::bsrr_word(KICKOS_BOARD_LED_BIT, true);
    }
    else
    {
        r32(bsrr) = stm32::bsrr_word(KICKOS_BOARD_LED_BIT, false);
    }
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

// Read-only branch-clock oracle (KOS_SYS_PERIPH_CLOCK_HZ). USART2 is on APB1 (RM0383 sec.2.3
// Table 1), whose rate clock_init() landed in pclk1_hz, so a userspace UART driver derives
// its own divisor instead of carrying a copy of the PLL plan. 0 for every other block: a
// WRONG branch clock silently garbles the wire, so an unknown must not answer with the core
// clock. USART1/USART6 are APB2 and would need their own entry, which no composition grants.
uint32_t arch_periph_clock_hz(uintptr_t base)
{
    if (base == mmap::USART2_BASE)
    {
        return pclk1_hz;
    }
    return 0u;
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
