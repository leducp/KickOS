// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// GICv2 backend for the arm64 family: the half ARM architects. The chip's kickos_gicv2
// (gicv2.h) supplies the distributor and CPU-interface bases, the INTID count and the INTID
// the EL1 physical timer asserts.

#include <kickos/arch/arch.h>

#include "gicv2.h"

#include <kickos/sys/atomic.h>

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
    constexpr uintptr_t GICD_SGIR = 0xF00;
    constexpr uintptr_t GICD_CPENDSGIR = 0xF10;
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
    // GICD_ITARGETSR is one byte of core bits and GICD_SGIR's CPUTargetList the same eight, so
    // eight cores is the whole space this controller addresses.
    static_assert(KICKOS_NUM_CORES <= 8, "GICv2 ITARGETSR targets at most 8 cores");

    // The doorbell's INTID. Legal range for a GICD_SGIR write is 0 to 15.
    constexpr int GIC_SGI_DOORBELL = 0;
    static_assert(GIC_SGI_DOORBELL >= 0 and GIC_SGI_DOORBELL <= 15,
                  "GICD_SGIR carries a 4-bit INTID");

    // EACH CORE'S GICD_SGIR TARGET BIT, WHICH IS A GIC CPU INTERFACE NUMBER AND NOT A CORE
    // INDEX. IHI 0048B.b's one discovery mechanism: a read of a CPU-targets field of
    // GICD_ITARGETSR0-7 returns the number of the processor performing the read.
    kickos::Atomic<uint8_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>
        g_target_bit[KICKOS_NUM_CORES] = {};
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

#if KICKOS_NUM_CORES > 1
    // GROUP 0, WHICH IS WHAT THE ACKNOWLEDGE PATH READS. A group mismatch drops a
    // software-generated interrupt silently, so this INTID keeps the group GICD_IGROUPR0
    // resets it to and GICD_SGIR.NSATT stays clear to match. Without security extensions
    // IGROUPR is RAZ/WI and there is one group; with them, GICC_IAR and GICC_EOIR here are
    // group 0's.
    //
    // GICD_ICPENDR writes are ignored for INTIDs below 16; an SGI's pending state is
    // GICD_CPENDSGIR's. Every source bit of this core's byte, clearing a raise latched before
    // this core owned an interface.
    *gicd32(GICD_CPENDSGIR + (GIC_SGI_DOORBELL / 4) * 4)
        = 0xFFu << ((GIC_SGI_DOORBELL % 4) * 8);
    *gicd8(GICD_IPRIORITYR + static_cast<uintptr_t>(GIC_SGI_DOORBELL)) = 0;
    *gicd32(GICD_ISENABLER + (GIC_SGI_DOORBELL / 32) * 4) = 1u << (GIC_SGI_DOORBELL % 32);

    // GICD_ITARGETSR0-7 are read-only and banked, so this byte is this core's own interface
    // number. Published last, so a sender that finds the bit reaches a live interface.
    g_target_bit[arch_cpu_id()] = *gicd8(GICD_ITARGETSR);
#endif
}

#if KICKOS_NUM_CORES > 1
// ONE WRITE REACHES ANY SUBSET: TargetListFilter 0b00 in [25:24] uses CPUTargetList in
// [23:16], NSATT in [15] stays clear for group 0, and the INTID sits in [3:0]. GICD_SGIR is
// write-only.
//
// A core whose target bit is unpublished contributes nothing to the list: its interface number
// is unknown, and bit `index` would be a bet on interface numbering.
void kickos_gicv2_doorbell_send(uint32_t cores)
{
    uint32_t list = 0;
    for (uint32_t index = 0; index < KICKOS_NUM_CORES; index++)
    {
        if ((cores & (1u << index)) != 0)
        {
            list |= g_target_bit[index];
        }
    }
    if (list == 0)
    {
        return;
    }
    // IHI 0048B.b SPECIFIES NO ORDERING FOR SGI GENERATION, so the far side's view of earlier
    // writes is the memory model's to order.
    __asm volatile("dsb ish" ::: "memory");
    *gicd32(GICD_SGIR) = ((list & 0xFFu) << 16) | static_cast<uint32_t>(GIC_SGI_DOORBELL);
}

// Write-1-to-clear over all eight source bits of this core's byte. The pending state of an SGI
// is per target core AND per source core, and GICD_CPENDSGIR is banked to the accessing core.
void kickos_gicv2_doorbell_clear(void)
{
    *gicd32(GICD_CPENDSGIR + (GIC_SGI_DOORBELL / 4) * 4)
        = 0xFFu << ((GIC_SGI_DOORBELL % 4) * 8);
}

// This core's banked enable word, doorbell bit excepted. Word 0 covers the SGI and PPI IDs,
// which are the only ones banked.
void kickos_gicv2_doorbell_only(void)
{
    *gicd32(GICD_ICENABLER) = ~(1u << (GIC_SGI_DOORBELL % 32));
}
#endif

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
        // Core zero's own PUBLISHED interface number; the literal 0x01 names interface zero.
#if KICKOS_NUM_CORES > 1
        *gicd8(GICD_ITARGETSR + static_cast<uintptr_t>(line)) = g_target_bit[0];
#else
        *gicd8(GICD_ITARGETSR + static_cast<uintptr_t>(line)) = 0x01;
#endif
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
#if KICKOS_NUM_CORES > 1
    else if (intid == static_cast<uint32_t>(GIC_SGI_DOORBELL))
    {
        // SGIs are edge-triggered (GICD_ICFGR0's SGI config bits are RAO/WI), and the
        // acknowledge above cleared the pending state.
        kickos_arm64_doorbell_service();
    }
#endif
    else
    {
        kickos_isr_irq(static_cast<int>(intid));
    }
    // THE WHOLE VALUE READ: GICC_EOIR must carry back the CPUID that GICC_IAR[12:10] reported
    // for an SGI, and a PPI or SPI reports zero there.
    *gicc32(GICC_EOIR) = iar;
#if KICKOS_KERNEL_CORES > 1
    // AFTER THE END OF INTERRUPT AND OUTSIDE THE SERVICE BODY: this takes the kernel lock,
    // which the service body may not, the service answering an initiator that may hold it.
    //
    // THE CELL IS THE AUTHORITY, NOT THE RAISE: the doorbell also carries rendezvous whose
    // targets owe no scheduler entry, and the take is what tells the two apart.
    if (intid == static_cast<uint32_t>(GIC_SGI_DOORBELL)
        and kickos_kernel_core_resched_take() != 0)
    {
        kickos_kernel_core_resched();
    }
#endif
}

}
