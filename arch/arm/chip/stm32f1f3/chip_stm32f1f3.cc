// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bodies every STM32 F0/F1/F3 chip backend spells the same way: the PLL bring-up,
// the monotonic clock over a free-running counter, and the buffered USART console.
// Everything a part answers differently reaches here as a compile-time constant or an
// inline read from its own family_map.h, so this compiles per chip into that chip's
// archive and folds to what the chip translation unit emitted before.

#include <kickos/arch/arch.h>
#include <kickos/arch/clk_anchor.h> // shared tickless-clock epoch anchor (B2)
#include <kickos/config/limits.h>
#include <kickos/console_tx.h>

#include "family_map.h" // the chip's own answers, on its include path
#include "stm32f1f3.h"

#include <stdint.h>

namespace
{
    using namespace kickos::stm32;

    // Bounded ready-flag poll: true once (reg & mask) == mask, false if the bit never
    // sets within the budget, so a dead crystal degrades the boot to the reset clock
    // instead of hanging it.
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

    // Software 64-bit extension of the chip's 32-bit counter. The wrap is folded either
    // by a thread read or, when the system is idle with the tickless timer disarmed, by
    // the overflow ISR below, exactly once: whoever reads first advances g_clk_last, so
    // the other sees no backward step. Without that ISR a wrap across a fully quiescent
    // idle would be lost. The two words are ONE value, and the crit section is what keeps
    // them coherent against the ISR, not the atomicity of either word.
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
            // A counter chained out of two 16-bit halves admits a master-wrap before
            // slave-increment SKEW, reading up to one low half BELOW the last value. A
            // magnitude discriminator tells that tear from a real wrap (gap ~2^32) and
            // clamps it, since reading a tear as a wrap leaps the clock a whole period.
            // A counter that is one 32-bit register cannot tear, and there a backward
            // step of any size is a wrap.
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

    // --- Buffered console TX backend (console_tx.h). The ring drains via the USART TXE
    // (TX-data-register-empty) interrupt, which is level-triggered: arming TXEIE while
    // TXE is set raises it immediately. ---
    int stm32_tx_slot_free(void) { return (reg32(chip::USART_SR) & chip::USART_TXE) != 0; }
    void stm32_tx_push(uint8_t b) { reg32(chip::USART_DR) = b; }
    void stm32_tx_irq_enable(void) { reg32(chip::USART_CR1) |= chip::CR1_TXEIE; }
    void stm32_tx_irq_disable(void) { reg32(chip::USART_CR1) &= ~chip::CR1_TXEIE; }
    char console_tx_buf[KICKOS_CONSOLE_TX_SIZE];
    console_tx_backend const stm32_console_backend = {
        stm32_tx_slot_free, stm32_tx_push, stm32_tx_irq_enable, stm32_tx_irq_disable};
}

namespace kickos::stm32
{
    bool clock_init()
    {
        // Flash wait states go in BEFORE the clock rises, else an instruction fetch at
        // the new rate fails. More states than the rate needs is always safe.
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

// Monotonic clock: free-running counter ticks -> ns, the required per-chip
// arch_clock_now. Pure epoch read: the anchor holds the rate, so no divide and no rate
// derivation happens here.
uint64_t arch_clock_now(void)
{
    return g_clk.ns_from(clk_ticks());
}

// Counter-overflow ISR, vectored by each chip's startup.S. Observes the wrap while the
// tickless timer is disarmed and no thread reads the clock; clk_ticks folds it into
// g_clk_high (idempotent against a concurrent thread read). Runs in the maskable band,
// so an IrqLock defers it harmlessly.
void kickos_stm32_clock_isr(void)
{
    reg32(chip::CLK_TIMER_SR) = ~TIM_SR_UIF; // ack the update flag (rc_w0)
    clk_ticks();
}

int arch_console_write(char const* buf, size_t n)
{
    return console_tx_insert_line(buf, n, KICKOS_CONSOLE_CRLF);
}

void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((reg32(chip::USART_SR) & chip::USART_TXE) == 0)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        reg32(chip::USART_DR) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = KICKOS_CONSOLE_TX_SIZE;
    *irq_line = chip::CONSOLE_IRQ;
    return &stm32_console_backend;
}

}
