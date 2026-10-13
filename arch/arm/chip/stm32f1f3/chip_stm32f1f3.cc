// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bodies every STM32 F0/F1/F3 chip backend shares: the PLL bring-up, the monotonic clock over
// a free-running counter, and the buffered USART console. What a part answers differently reaches
// here as a compile-time constant or inline read from its own family_map.h.

#include <kickos/arch/arch.h>
#include <kickos/arch/console_poll.h>
#include <kickos/arch/clk_anchor.h>
#include <kickos/config/limits.h>
#include <kickos/console_tx.h>

#include "family_map.h"
#include "stm32f1f3.h"

#include <stdint.h>

namespace
{
    using namespace kickos::stm32;

    // False if the bit never sets within the budget, so a dead crystal degrades the boot to the
    // reset clock instead of hanging it.
    bool poll_set(uintptr_t reg, uint32_t mask)
    {
        for (uint32_t i = 0; i < chip::POLL_LIMIT; i++)
        {
            if ((reg32(reg) & mask) == mask)
            {
                return true;
            }
        }
        return false;
    }

    // Software 64-bit extension of the 32-bit counter. The wrap is folded exactly once, by a
    // thread read or by the overflow ISR below: whoever reads first advances g_clk_last. Without
    // that ISR a wrap across a quiescent idle would be lost. The two words are ONE value; the
    // crit section keeps them coherent against the ISR.
    uint32_t g_clk_high = 0;
    uint32_t g_clk_last = 0;

    kickos::arch_clk_anchor g_clk;

    uint64_t clk_ticks()
    {
        arch_irq_state_t s = arch_irq_save();
        uint32_t cur = chip::clk_counter();
        uint32_t const last = g_clk_last;
        if (cur < last)
        {
            // A counter chained out of two 16-bit halves can SKEW, reading up to one low half
            // BELOW the last value. A magnitude test tells that tear from a real wrap (gap ~2^32)
            // and clamps it: a tear read as a wrap leaps the clock a whole period. A single
            // 32-bit register cannot tear, so there any backward step is a wrap.
            if (not chip::CLK_COUNTER_TEARS or last - cur > 0x80000000u)
            {
                ++g_clk_high;
            }
            else
            {
                cur = last;
            }
        }
        g_clk_last = cur;
        uint64_t high = g_clk_high;
        arch_irq_restore(s);
        return (high << 32) | cur;
    }

    // --- Buffered console TX backend. TXE is level-triggered: arming TXEIE while TXE is set
    // raises the IRQ at once. ---
    int stm32_tx_slot_free(void) { return (reg32(chip::USART_SR) & chip::USART_TXE) != 0; }
    void stm32_tx_push(uint8_t b) { reg32(chip::USART_DR) = b; }
    void stm32_tx_irq_enable(void) { reg32(chip::USART_CR1) |= chip::CR1_TXEIE; }
    void stm32_tx_irq_disable(void) { reg32(chip::USART_CR1) &= ~chip::CR1_TXEIE; }
    console_tx_backend const stm32_console_backend = {
        stm32_tx_slot_free, stm32_tx_push, stm32_tx_irq_enable, stm32_tx_irq_disable,
        &kickos_console_tx_storage};
}

namespace kickos::stm32
{
    bool clock_init()
    {
        // Wait states BEFORE the clock rises, else a fetch at the new rate fails; more than the
        // rate needs is safe.
        uint32_t acr = reg32(FLASH_ACR);
        acr &= ~ACR_LATENCY_MASK;
        acr |= chip::FLASH_LATENCY | ACR_PRFTBE;
        reg32(FLASH_ACR) = acr;

        reg32(RCC_CR) |= chip::PLL_SRC_ON;
        if (not poll_set(RCC_CR, chip::PLL_SRC_RDY))
        {
            return false;
        }

        // PLLSRC/PLLMUL are writable only while the PLL is off, which it is at reset.
        uint32_t cfgr = reg32(RCC_CFGR);
        cfgr &= ~chip::CFGR_CLEAR;
        cfgr |= chip::CFGR_SET;
        reg32(RCC_CFGR) = cfgr;

        reg32(RCC_CR) |= CR_PLLON;
        if (not poll_set(RCC_CR, CR_PLLRDY))
        {
            return false;
        }

        uint32_t sw = reg32(RCC_CFGR);
        sw &= ~CFGR_SW_MASK;
        sw |= CFGR_SW_PLL;
        reg32(RCC_CFGR) = sw;
        for (uint32_t i = 0; i < chip::POLL_LIMIT; i++)
        {
            if ((reg32(RCC_CFGR) & CFGR_SWS_MASK) == CFGR_SWS_PLL)
            {
                return true;
            }
        }
        return false;
    }

    void clock_anchor(uint32_t hz)
    {
        g_clk.init(hz);
    }
}

extern "C"
{

// Must stay a pure read: deriving the rate here would reprice all elapsed history.
uint64_t arch_clock_now(void)
{
    return g_clk.ns_from(clk_ticks());
}

// Observes the wrap while no thread reads the clock; clk_ticks folds it idempotently against a
// concurrent read. Runs in the maskable band, so an IrqLock defers it harmlessly.
void kickos_stm32_clock_isr(void)
{
    reg32(chip::CLK_TIMER_SR) = ~TIM_SR_UIF; // ack the update flag (rc_w0)
    clk_ticks();
}

bool arch_console_write_sync(char const* buf, size_t n)
{
    return kickos::console_poll::write<stm32_tx_slot_free, stm32_tx_push>(buf, n);
}

console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
    *irq_line = chip::CONSOLE_IRQ;
    return &stm32_console_backend;
}

}
