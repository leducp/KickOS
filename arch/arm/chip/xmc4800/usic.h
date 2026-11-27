// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Infineon XMC4800 USIC operations common to every protocol (ASC/SSC/IIC/IIS), keyed
// on the channel base address.

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_USIC_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_USIC_H

#include "regs/scu.h"
#include "regs/usic.h"
#include <kickos/chip_mmap.h>
#include <kickos/io/mmio.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
namespace xmc
{
namespace usic
{
    using namespace kickos::xmc::reg::usic;

    // A module's two channels, and the SCU gate and reset bit that holds them both.
    struct Module
    {
        uintptr_t ch0;
        uintptr_t cgatclr;
        uintptr_t prclr;
        uint32_t bit;
    };

    inline constexpr Module MODULES[] = {
        {mmap::USIC0_CH0_BASE, reg::scu::CGATCLR0, reg::scu::PRCLR0, reg::scu::USIC0_GATE_BIT},
        {mmap::USIC1_CH0_BASE, reg::scu::CGATCLR1, reg::scu::PRCLR1, reg::scu::USIC1_GATE_BIT},
        {mmap::USIC2_CH0_BASE, reg::scu::CGATCLR1, reg::scu::PRCLR1, reg::scu::USIC2_GATE_BIT},
    };

    inline constexpr uintptr_t CHANNEL_STRIDE = mmap::USIC0_CH1_BASE - mmap::USIC0_CH0_BASE;

    // The module whose channel sits at exactly `base`; bit 0 where no channel does.
    constexpr Module module_of(uintptr_t base)
    {
        for (Module const& m : MODULES)
        {
            if (base == m.ch0 or base == m.ch0 + CHANNEL_STRIDE)
            {
                return m;
            }
        }
        return {0, 0, 0, 0};
    }

    // The point for a branch clock, or false where no point covers it. Keyed on fPERIPH
    // and not on fCPU: PBCLKCR.PBDIV is written by clock_init AFTER both of its degrade
    // returns, so the two rates stop being a fixed ratio exactly on the path that needs
    // this most.
    bool baud_for_periph(uint32_t periph_hz, Baud* out);

    // Ungate then de-reset the module via SCU. Ordering is mandatory (RM 11.6:
    // clock must be active before the individual peripheral reset is released).
    void module_clock_enable(uintptr_t cgatclr, uintptr_t prclr, uint32_t bit);

    // Enable the module kernel clock (KSCFG MODEN|BPMODEN) with the RM-recommended
    // read-back before any further USIC register access (RM p.18-165).
    void kernel_clock_enable(uintptr_t base);

    void set_baud(uintptr_t base, Baud const& b);

    // Select an input line for input stage DXn (DXnCR.DSEL); dxn_off is off::DXnCR.
    void select_input(uintptr_t base, uintptr_t dxn_off, uint32_t dsel);

    // Partition the shared FIFO RAM for a channel region. ctr_off is off::TBCTR
    // (TX) or off::RBCTR (RX); size uses the regs/usic.h SIZE coding (0 disables).
    void configure_fifo(uintptr_t base, uintptr_t ctr_off, uint32_t dptr, uint32_t size, uint32_t limit);

    // Raw TX status/data (single-shot, no polling loop; the caller bounds waits).
    void tx_put(uintptr_t base, uint8_t v);
    bool tx_idle(uintptr_t base);  // PSR.BUSY clear: shifter has emptied

    // Transmit-buffer interrupt gate (buffered console drain trigger). tx_irq_route
    // selects the service-request output SRx (0..5) once at init; enable/disable
    // toggle CCR.TBIEN while the channel runs.
    void tx_irq_route(uintptr_t base, uint32_t srx);
    void tx_irq_enable(uintptr_t base);
    void tx_irq_disable(uintptr_t base);
}
}
}

#endif
