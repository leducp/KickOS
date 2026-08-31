// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// GICv2 backend for the arm64 family: the half ARM architects. The chip's kickos_gicv2
// (gicv2.h) supplies the distributor and CPU-interface bases, the INTID count and the INTID
// the EL1 physical timer asserts.

#include <kickos/arch/arch.h>

#include "gicv2.h"

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    // VA - PA for the kernel's half, defined by every arm64 chip linker script.
    extern unsigned char __kickos_arm64_va_base[];
}

namespace
{
    // EVERY DEVICE REGISTER IS REACHED THROUGH THE KERNEL'S OWN HALF. The device gigabyte is
    // mapped at PA + __kickos_arm64_va_base by TTBR1, which every address space shares; TTBR0
    // carries a per-process root that maps no device at all, so a low literal here would
    // translate against whatever process happened to be running.
    inline uintptr_t dev_va(uintptr_t pa)
    {
        return pa + reinterpret_cast<uintptr_t>(__kickos_arm64_va_base);
    }

    inline volatile uint32_t* gicd32(uintptr_t off)
    {
        return reinterpret_cast<volatile uint32_t*>(dev_va(kickos_gicv2.dist_pa + off));
    }

    inline volatile uint8_t* gicd8(uintptr_t off)
    {
        return reinterpret_cast<volatile uint8_t*>(dev_va(kickos_gicv2.dist_pa + off));
    }

    inline volatile uint32_t* gicc32(uintptr_t off)
    {
        return reinterpret_cast<volatile uint32_t*>(dev_va(kickos_gicv2.cpu_pa + off));
    }

    // Displacements from the two bases.
    constexpr uintptr_t GICD_CTLR = 0x000;
    constexpr uintptr_t GICD_ISENABLER = 0x100;
    constexpr uintptr_t GICD_ICENABLER = 0x180;
    constexpr uintptr_t GICD_ISPENDR = 0x200;
    constexpr uintptr_t GICD_ICPENDR = 0x280;
    constexpr uintptr_t GICD_IPRIORITYR = 0x400;
    constexpr uintptr_t GICD_ITARGETSR = 0x800;
    constexpr uintptr_t GICC_CTLR = 0x000;
    constexpr uintptr_t GICC_PMR = 0x004;
    constexpr uintptr_t GICC_IAR = 0x00C;
    constexpr uintptr_t GICC_EOIR = 0x010;

    // THE KIND, as a value range rather than a separate field (roadmap.md's `(line, kind)`).
    // Below 32 the GIC banks its registers per core, so INTID 30 names THIS core's timer; at
    // or above 32 an interrupt is global and reaches no core until ITARGETSR names one. Every
    // arch_irq_* body branches on this boundary and nothing else does.
    constexpr int GIC_BANKED_INTIDS = 32;

    // INTID 1023 means "no pending interrupt" in an IAR read.
    constexpr uint32_t GICC_IAR_SPURIOUS = 1023;
    constexpr uint32_t GICC_IAR_ID_MASK = 0x3FF;

#if KICKOS_NUM_CORES > 1
    // GICD_ITARGETSR is one byte of core bits, so eight cores is the whole space this
    // controller addresses and a ninth has no target encoding.
    static_assert(KICKOS_NUM_CORES <= 8, "GICv2 ITARGETSR targets at most 8 cores");
#endif
}

extern "C"
{

// The distributor's SHARED half, one write set for the machine. GICD_CTLR and every register
// at or above GIC_BANKED_INTIDS answer the same for every CPU interface, so a second core
// repeating this would overwrite the first core's answers.
void kickos_gicv2_dist_init(void)
{
    *gicd32(GICD_CTLR) = 0;
    // arch.h's reset contract: every line starts MASKED. The first word is the running core's
    // own bank and belongs with the CPU interface, so the sweep starts at the first shared ID.
    for (int intid = GIC_BANKED_INTIDS; intid < kickos_gicv2.intid_count; intid += 32)
    {
        *gicd32(GICD_ICENABLER + (intid / 32) * 4) = 0xFFFFFFFFu;
        *gicd32(GICD_ICPENDR + (intid / 32) * 4) = 0xFFFFFFFFu;
    }
    *gicd32(GICD_CTLR) = 1;
}

// This core's hardware edge alone. GICC_CTLR and GICC_PMR are its memory-mapped CPU interface,
// and the distributor registers below GIC_BANKED_INTIDS are its own bank of them.
void kickos_gicv2_percore_init(void)
{
    *gicc32(GICC_CTLR) = 0;
    // arch.h's reset contract over this core's banked lines. The sweep covers the SGI IDs, so
    // a cross-core doorbell must enable its own INTID after this has run.
    *gicd32(GICD_ICENABLER) = 0xFFFFFFFFu;
    *gicd32(GICD_ICPENDR) = 0xFFFFFFFFu;
    // PMR wide open at 0xF0: everything here sits at priority 0, and the reset PMR of 0
    // blocks all of it.
    *gicc32(GICC_PMR) = 0xF0;
    *gicc32(GICC_CTLR) = 1;

    // Banked, so no target is owed: the timer PPI named from this core is this core's own.
    int const timer = kickos_gicv2.timer_intid;
    *gicd8(GICD_IPRIORITYR + static_cast<uintptr_t>(timer)) = 0;
    *gicd32(GICD_ISENABLER + (timer / 32) * 4) = 1u << (timer % 32);
}

// Write-1-to-ACT, so the pending state of one INTID drops with a single aligned store.
void kickos_gicv2_clear_pending(int intid)
{
    *gicd32(GICD_ICPENDR + (intid / 32) * 4) = 1u << (intid % 32);
}

// --- Interrupt controller: the GIC behind the mask/unmask/clear triad -------
// Self-bracketed per arch.h at no cost: the enable and pending registers are write-1-to-ACT,
// so no body here read-modify-writes and every store is single and aligned. Unmask writes the
// ENABLE last, so a half-applied sequence leaves the line masked.
//
// WHICH CORE THIS FAMILY ACTS ON IS THE INTID'S. A line below GIC_BANKED_INTIDS reaches the
// calling core's own bank, so the same argument means a different interrupt on each core; a
// line at or above it is global and arch_irq_unmask pins it to core 0.
void arch_irq_mask(int line)
{
    if (line < 0 or line >= kickos_gicv2.intid_count)
    {
        return;
    }
    *gicd32(GICD_ICENABLER + (line / 32) * 4) = 1u << (line % 32);
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= kickos_gicv2.intid_count)
    {
        return;
    }
    // BYTE per INTID: a word index programs a different interrupt, and a 32-bit access is
    // also unaligned, which on Device memory faults.
    *gicd8(GICD_IPRIORITYR + static_cast<uintptr_t>(line)) = 0;
    if (line >= GIC_BANKED_INTIDS)
    {
        // A global interrupt reaches no core until one is named; a banked one needs none.
        *gicd8(GICD_ITARGETSR + static_cast<uintptr_t>(line)) = 0x01;
    }
    *gicd32(GICD_ISENABLER + (line / 32) * 4) = 1u << (line % 32);
}

void arch_irq_clear_pending(int line)
{
    if (line < 0 or line >= kickos_gicv2.intid_count)
    {
        return;
    }
    kickos_gicv2_clear_pending(line);
}

// Test scaffolding (arch.h). ISPENDR pends in the controller, so delivery takes the ordinary
// path and the latch-while-masked contract needs no software shadow.
void arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= kickos_gicv2.intid_count)
    {
        return;
    }
    *gicd32(GICD_ISPENDR + (irq / 32) * 4) = 1u << (irq % 32);
}

// One interrupt per entry: the GIC signals again for anything still pending.
void kickos_armv8a_gic_dispatch(void)
{
    uint32_t const iar = *gicc32(GICC_IAR);
    uint32_t const intid = iar & GICC_IAR_ID_MASK;
    if (intid == GICC_IAR_SPURIOUS)
    {
        return; // no EOI is owed for a spurious read
    }
    if (intid == static_cast<uint32_t>(kickos_gicv2.timer_intid))
    {
        // The output is LEVEL, and kickos_isr_timer's re-arm or disarm is what lowers it;
        // an EOI alone would re-enter here forever.
        kickos_isr_timer();
    }
    else
    {
        kickos_isr_irq(static_cast<int>(intid));
    }
    *gicc32(GICC_EOIR) = iar;
}

}
