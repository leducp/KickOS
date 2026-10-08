// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Infineon XMC4800 USIC operations common to every protocol (ASC/SSC/IIC/IIS), keyed
// on the channel base address.

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_USIC_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_USIC_H

#include "regs/usic.h"

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
namespace xmc
{
namespace usic
{
    inline volatile uint32_t& reg32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    using namespace kickos::xmc::reg::usic;

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
    bool tx_ready(uintptr_t base); // TCSR.TDV clear: transmit buffer can take a word
    void tx_put(uintptr_t base, uint8_t v);
    bool tx_idle(uintptr_t base);  // PSR.BUSY clear: shifter has emptied

    // Transmit-buffer interrupt gate (buffered console drain trigger). tx_irq_route
    // selects the service-request output SRx (0..5) once at init; enable/disable
    // toggle CCR.TBIEN while the channel runs.
    void tx_irq_route(uintptr_t base, uint32_t srx);
    void tx_irq_enable(uintptr_t base);
    void tx_irq_disable(uintptr_t base);

    bool rx_ready(uintptr_t base); // a received word is waiting in the buffer
    uint8_t rx_get(uintptr_t base); // read + release the receive buffer

    // Read PSR, clear the masked flags via PSCR, return the masked bits that were set.
    uint32_t status_read_clear(uintptr_t base, uint32_t mask);
}
}
}

#endif
