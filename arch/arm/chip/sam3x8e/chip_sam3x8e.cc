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
#include <kickos/console_tx.h>
#include <kickos/io/mmio.h>
#include <kickos/sys/abi.h>

#include <kickos/chip_mmap.h>
#include "board_pins.h"
#include "irq.h"
#include <chip_layout.h>
#include "regs.h"
#include "uart_baud.h"

#include <stdint.h>

namespace
{
    constexpr uint32_t MAINCK_RC_HZ = 4000000u;
    constexpr uint32_t MAINCK_XTAL_HZ = 12000000u;
}

extern "C"
{
    void kickos_armv7m_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

    // PMC_MCKR comes out of reset selecting MAINCK undivided (sec.28.15.11, reset 0x1) and
    // MAINCK is the fast RC, so MCK is this until clock_init moves one of the two terms.
    uint32_t SystemCoreClock = MAINCK_RC_HZ;
}

namespace
{
    namespace mmap = kickos::sam3x8e::mmap;
    namespace irq = kickos::sam3x8e::irq;
    static_assert(irq::TC0_IRQ == KICKOS_LAYOUT_LINE_TC0_IRQ,
                  "startup.S vectors the clock-wrap ISR at KICKOS_LAYOUT_LINE_TC0_IRQ");

    constexpr uintptr_t WDT_MR = mmap::WDT_BASE + 0x04; // write-once
    constexpr uint32_t WDT_MR_WDDIS = 1u << 15;

    // EEFC (sec.18): the two flash banks. FWS=4 covers up to 90 MHz (sec.45) and is required for
    // 84 MHz; set BEFORE the clock is raised.
    constexpr uintptr_t EEFC0_FMR = mmap::EEFC0_BASE + 0x00;
    constexpr uintptr_t EEFC1_FMR = mmap::EEFC1_BASE + 0x00;
    constexpr uint32_t FMR_FWS_4 = 4u << 8;

    constexpr uintptr_t CKGR_MOR = mmap::PMC_BASE + 0x20;
    constexpr uintptr_t CKGR_PLLAR = mmap::PMC_BASE + 0x28;
    constexpr uintptr_t PMC_MCKR = mmap::PMC_BASE + 0x30;
    constexpr uintptr_t PMC_SR = mmap::PMC_BASE + 0x68;

    // CKGR_MOR (sec.28): KEY 0x37 gates the write; MOSCXTST is the crystal startup counter in
    // SLCK/8; the fast RC (MOSCRCEN) stays on while the crystal warms up, then MOSCSEL picks the
    // crystal.
    //
    // MOSCXTS asserts when the MOSCXTST counter expires, NOT on physical crystal detection, so
    // MOSCXTST MUST cover the crystal's worst-case warm-up or PLLA locks on a still-settling
    // MAINCK (the intermittent-boot race). SLCK is the slow RC, 22..42 kHz, so the count is sized
    // at the FAST end: 0x80 gives 24.4 ms at 42 kHz and 46.5 ms at 22 kHz, past the ~15 ms
    // crystal spec. Unconfirmed on silicon: do not trim toward 15 ms, 0x50 has no margin at
    // 42 kHz.
    constexpr uint32_t MOR_KEY = 0x37u << 16;
    constexpr uint32_t MOR_MOSCXTEN = 1u << 0;
    constexpr uint32_t MOR_MOSCRCEN = 1u << 3;
    constexpr uint32_t MOR_MOSCXTST = 0x80u << 8;
    constexpr uint32_t MOR_MOSCSEL = 1u << 24;
    constexpr uint32_t MOR_CRYSTAL = MOR_KEY | MOR_MOSCXTST | MOR_MOSCRCEN | MOR_MOSCXTEN;

    // PLLA = MAINCK * (MULA+1) / DIVA. ONE (bit 29) reads 1; PLLCOUNT is the LOCK delay in SLCK.
    constexpr uint32_t PLLA_MUL = 14u;
    constexpr uint32_t PLLA_DIV = 1u;
    constexpr uint32_t PLLA_HZ = MAINCK_XTAL_HZ * PLLA_MUL / PLLA_DIV;
    constexpr uint32_t PLLAR_ONE = 1u << 29;
    constexpr uint32_t PLLAR_MULA = (PLLA_MUL - 1u) << 16;
    constexpr uint32_t PLLAR_COUNT = 0x3Fu << 8;
    constexpr uint32_t PLLAR_DIVA = PLLA_DIV << 0;

    constexpr uint32_t MCKR_CSS_MASK = 3u << 0;
    constexpr uint32_t MCKR_CSS_MAIN = 1u << 0;
    constexpr uint32_t MCKR_CSS_PLLA = 2u << 0;
    constexpr uint32_t MCKR_PRES_MASK = 7u << 4;
    constexpr uint32_t MCKR_PRES_DIV2 = 1u << 4;

    constexpr uint32_t MCKR_MAIN = MCKR_CSS_MAIN;
    constexpr uint32_t MCKR_MAIN_DIV2 = MCKR_PRES_DIV2 | MCKR_CSS_MAIN;
    constexpr uint32_t MCKR_PLLA_DIV2 = MCKR_PRES_DIV2 | MCKR_CSS_PLLA;

    // PRES DIVIDES WHATEVER CSS SELECTED, so PRES_DIV2 beside CSS_MAIN is the crystal halved,
    // not PLLA halved. A CSS this backend never writes answers 0, which a divisor cannot
    // mistake for a working clock.
    constexpr uint32_t mck_for(uint32_t mckr, uint32_t mainck)
    {
        uint32_t src = 0u;
        if ((mckr & MCKR_CSS_MASK) == MCKR_CSS_MAIN)
        {
            src = mainck;
        }
        else if ((mckr & MCKR_CSS_MASK) == MCKR_CSS_PLLA)
        {
            src = PLLA_HZ;
        }
        uint32_t div = 1u;
        if ((mckr & MCKR_PRES_MASK) == MCKR_PRES_DIV2)
        {
            div = 2u;
        }
        return src / div;
    }

    // The part's own maximum, and the bound FWS=4 was chosen against.
    constexpr uint32_t MCK_MAX_HZ = 84000000u;

    static_assert(mck_for(MCKR_MAIN, MAINCK_XTAL_HZ) == MAINCK_XTAL_HZ,
                  "CSS_MAIN with no PRES is MAINCK undivided");
    static_assert(mck_for(MCKR_MAIN_DIV2, MAINCK_XTAL_HZ) == 6000000u,
                  "PRES_DIV2 beside CSS_MAIN halves the CRYSTAL: the intermediate state of "
                  "the PLL switch is 6 MHz, not the 12 MHz the crystal runs at");
    static_assert(mck_for(MCKR_PLLA_DIV2, MAINCK_XTAL_HZ) == MCK_MAX_HZ,
                  "PLLA over the MCKR prescaler must land on the SAM3X8E maximum MCK; the "
                  "flash wait states are sized for that bound too");

    // The only writer of PMC_MCKR, so SystemCoreClock always names the live selection.
    void mckr_select(uint32_t mckr, uint32_t mainck);

    constexpr uint32_t SR_MOSCXTS = 1u << 0;
    constexpr uint32_t SR_LOCKA = 1u << 1;
    constexpr uint32_t SR_MCKRDY = 1u << 3;
    constexpr uint32_t SR_MOSCSELS = 1u << 16;

    // ~1M spins on the reset 4 MHz RC is hundreds of ms, well past the MOSCXTST window and
    // every SLCK-counted delay. On false the source never came up: the caller MUST NOT
    // select it (that is the boot race).
    bool pmc_wait(uint32_t bit)
    {
        for (uint32_t i = 0; i < 0x100000u; i++)
        {
            if ((r32(PMC_SR) & bit) != 0)
            {
                return true;
            }
        }
        return false;
    }

    // NO DEGRADE RETURN CARRIES A RATE OF ITS OWN: SystemCoreClock is restated at every edge
    // that moves MCK, and every failure just returns. PMC_MCKR resets to MAINCK undivided
    // (sec.28.15.11), so step 3's MOSCSEL moves MCK with no PMC_MCKR write of its own.
    void clock_init()
    {
        // 1. Flash wait states first, both banks; FWS=4 also covers the fallbacks.
        r32(EEFC0_FMR) = FMR_FWS_4;
        r32(EEFC1_FMR) = FMR_FWS_4;

        // 2. Start the 12 MHz crystal; the RC stays MAINCK meanwhile. No MOSCXTS: stay on the
        //    4 MHz RC, where the console cannot reach 115200 but the core and LED still run.
        r32(CKGR_MOR) = MOR_CRYSTAL;
        if (not pmc_wait(SR_MOSCXTS))
        {
            return;
        }

        // 3. Select the crystal as MAINCK, then run MCK off it before touching PLLA. The
        //    MOSCSEL switch is glitch-free and signals on MOSCSELS, not MCKRDY (sec.27.5.3),
        //    and CSS already names MAINCK, so MCK is on the crystal before the write below.
        r32(CKGR_MOR) = MOR_CRYSTAL | MOR_MOSCSEL;
        if (not pmc_wait(SR_MOSCSELS))
        {
            // MOSCSEL moves MCK with no PMC_MCKR write, so a switch that landed without
            // reporting would leave MCK on the crystal while the rate below still reads the
            // RC. Deselect before returning, and the asserted rate is true either way.
            r32(CKGR_MOR) = MOR_CRYSTAL;
            return;
        }
        mckr_select(MCKR_MAIN, MAINCK_XTAL_HZ);
        if (not pmc_wait(SR_MCKRDY))
        {
            return;
        }

        // 4. PLLA. If it never locks, MCK is already stable on the crystal, so stay there.
        r32(CKGR_PLLAR) = PLLAR_ONE | PLLAR_MULA | PLLAR_COUNT | PLLAR_DIVA;
        if (not pmc_wait(SR_LOCKA))
        {
            return;
        }

        // 5. Switch MCK to PLLA/2. A PLL source needs PRES set, MCKRDY, then CSS set, MCKRDY
        //    (sec.28.12). THE FIRST WRITE HALVES MAINCK, so the intermediate state is 6 MHz.
        mckr_select(MCKR_MAIN_DIV2, MAINCK_XTAL_HZ);
        if (not pmc_wait(SR_MCKRDY))
        {
            return;
        }
        mckr_select(MCKR_PLLA_DIV2, MAINCK_XTAL_HZ);
        if (not pmc_wait(SR_MCKRDY))
        {
            return;
        }
    }

    void mckr_select(uint32_t mckr, uint32_t mainck)
    {
        r32(PMC_MCKR) = mckr;
        SystemCoreClock = mck_for(mckr, mainck);
    }

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

    // 64-bit extension of TC_CV0, which wraps every ~102 s. The wrap is folded exactly once, by
    // whichever comes first of a thread read or the COVFS ISR; without that ISR a wrap across a
    // quiescent >102 s idle is lost. The two words are ONE value: the IrqLock in tc_ticks keeps
    // them coherent against the ISR.
    uint32_t g_clk_high = 0;
    uint32_t g_clk_last = 0;

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
        // A pend latched here redelivers one benign kickos_isr_timer tick on enable.
        arch_irq_unmask(irq::TC0_IRQ);
    }

    uint64_t tc_ticks()
    {
        arch_irq_state_t s = arch_irq_save();
        uint32_t cur = r32(TC0_CV0);
        if (cur < g_clk_last)
        {
            ++g_clk_high;
        }
        g_clk_last = cur;
        uint64_t hi = g_clk_high;
        arch_irq_restore(s);
        return (hi << 32) | cur;
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
    clock_init();
    tc_clock_init();
    // TC0 ch0 ticks at MCK/2.
    g_clk.init(SystemCoreClock / 2u);
    uart_init();
    kickos_armv7m_init();
}

// Must stay a pure read: deriving the rate here would reprice all elapsed history.
uint64_t arch_clock_now(void)
{
    return g_clk.ns_from(tc_ticks());
}

// Observes the wrap while no thread reads the clock. Runs in the maskable band, so an IrqLock
// defers it harmlessly.
void kickos_tc0_clock_isr(void)
{
    uint32_t drop = r32(TC0_SR0); // read-to-clear acks COVFS
    (void)drop;
    tc_ticks();
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
    kickos_crt_tail();
}

}
