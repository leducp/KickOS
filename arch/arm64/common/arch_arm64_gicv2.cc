// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// GICv2 backend for the arm64 family: the half ARM architects, behind gic.h. The chip's
// kickos_gicv2 (gicv2.h) supplies the distributor and CPU-interface bases, the INTID count
// and the INTID the EL1 physical timer asserts.

#include <kickos/arch/arch.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/chip_limits.h>

#include "gic.h"
#include "gicd.h"
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
    using namespace kickos::arm64;

    // Device registers are reached through the kernel's half (TTBR1): TTBR0's per-process root
    // maps no device.
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

    // Displacements from the two bases, beyond the shared distributor ones (gicd.h).
    constexpr uintptr_t GICD_ITARGETSR = 0x800;
    constexpr uintptr_t GICD_SGIR = 0xF00;
    constexpr uintptr_t GICD_CPENDSGIR = 0xF10;
    constexpr uintptr_t GICC_CTLR = 0x000;
    constexpr uintptr_t GICC_PMR = 0x004;
    constexpr uintptr_t GICC_IAR = 0x00C;
    constexpr uintptr_t GICC_EOIR = 0x010;

    // INTID 1023 means "no pending interrupt" in an IAR read.
    constexpr uint32_t GICC_IAR_SPURIOUS = 1023;
    constexpr uint32_t GICC_IAR_ID_MASK = 0x3FF;

#if KICKOS_NUM_CORES > 1
    // GICD_ITARGETSR bytes and GICD_SGIR's CPUTargetList are eight bits wide.
    static_assert(KICKOS_NUM_CORES <= 8, "GICv2 ITARGETSR targets at most 8 cores");

    // The doorbell's INTID. Legal range for a GICD_SGIR write is 0 to 15.
    constexpr int GIC_SGI_DOORBELL = 0;

    // Each core's GICD_SGIR target bit, a GIC CPU interface number and not a core index.
    // IHI 0048B.b's one discovery mechanism: a read of a CPU-targets field of GICD_ITARGETSR0-7
    // returns the number of the processor performing the read.
    kickos::Atomic<uint8_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>
        g_target_bit[KICKOS_NUM_CORES] = {};
#endif

    // The target byte arch_irq_route named for a global line; zero says none. Written on the
    // claiming core and read by an arch_irq_unmask that may run on another, so the release
    // store that seats a byte publishes the GICD_ITARGETSR write ahead of it.
    static_assert(KICKOS_MAX_IRQ > GIC_BANKED_INTIDS,
                  "a GIC with no global INTID leaves nothing to route");
    using RouteCell = kickos::Atomic<uint8_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>;
    RouteCell g_line_target[KICKOS_MAX_IRQ - GIC_BANKED_INTIDS] = {};

    // Null for a banked INTID and for one past the record.
    inline RouteCell* route_cell(int line)
    {
        if (line < GIC_BANKED_INTIDS or line >= KICKOS_MAX_IRQ)
        {
            return nullptr;
        }
        return &g_line_target[line - GIC_BANKED_INTIDS];
    }

    // The interface bit a core publishes for itself; zero while it has published none.
    inline uint8_t target_bit_of(uint32_t core)
    {
#if KICKOS_NUM_CORES > 1
        if (core >= KICKOS_NUM_CORES)
        {
            return 0;
        }
        return g_target_bit[core].load();
#else
        if (core != 0)
        {
            return 0;
        }
        return 0x01; // interface zero, the only one a single core has
#endif
    }
}

extern "C"
{

// The distributor's shared half: GICD_CTLR and every register at or above GIC_BANKED_INTIDS
// answer for every CPU interface, so a second core repeating this would overwrite the first's.
void kickos_armv8a_gic_dist_init(void)
{
    *gicd32(GICD_CTLR) = 0;
    // Every line starts masked (arch.h). The first word is the core's own bank, so the sweep
    // starts at the first shared ID.
    for (int intid = GIC_BANKED_INTIDS; intid < kickos_gicv2.intid_count; intid += 32)
    {
        *gicd32(GICD_ICENABLER + (intid / 32) * 4) = 0xFFFFFFFFu;
        *gicd32(GICD_ICPENDR + (intid / 32) * 4) = 0xFFFFFFFFu;
    }
    *gicd32(GICD_CTLR) = 1;
}

// This core's hardware edge alone: its CPU interface and its bank of the distributor registers
// below GIC_BANKED_INTIDS.
void kickos_armv8a_gic_percore_init(void)
{
    *gicc32(GICC_CTLR) = 0;
    // The sweep covers the SGI IDs, so a doorbell must enable its own INTID after this.
    *gicd32(GICD_ICENABLER) = 0xFFFFFFFFu;
    *gicd32(GICD_ICPENDR) = 0xFFFFFFFFu;
    // The reset PMR of 0 blocks priority 0, where every line here sits.
    *gicc32(GICC_PMR) = 0xF0;
    *gicc32(GICC_CTLR) = 1;

    // Banked: no target is owed.
    int const timer = kickos_gicv2.timer_intid;
    *gicd8(GICD_IPRIORITYR + static_cast<uintptr_t>(timer)) = 0;
    *gicd32(GICD_ISENABLER + (timer / 32) * 4) = 1u << (timer % 32);

#if KICKOS_NUM_CORES > 1
    // Group 0, which the acknowledge path reads: a group mismatch drops an SGI silently, so
    // this INTID keeps the group GICD_IGROUPR0 resets it to and GICD_SGIR.NSATT stays clear.
    //
    // GICD_ICPENDR ignores INTIDs below 16; an SGI's pending state is GICD_CPENDSGIR's. Every
    // source bit of this core's byte is cleared, dropping a raise latched before this core
    // owned an interface.
    *gicd32(GICD_CPENDSGIR + (GIC_SGI_DOORBELL / 4) * 4)
        = 0xFFu << ((GIC_SGI_DOORBELL % 4) * 8);
    *gicd8(GICD_IPRIORITYR + static_cast<uintptr_t>(GIC_SGI_DOORBELL)) = 0;
    *gicd32(GICD_ISENABLER + (GIC_SGI_DOORBELL / 32) * 4) = 1u << (GIC_SGI_DOORBELL % 32);

    // GICD_ITARGETSR0-7 are read-only and banked: this byte is this core's interface number.
    // Published last, so a sender that finds the bit reaches a live interface.
    g_target_bit[arch_cpu_id()] = *gicd8(GICD_ITARGETSR);
#endif
}

#if KICKOS_NUM_CORES > 1
// One write reaches any subset: TargetListFilter 0b00 in [25:24] uses CPUTargetList in
// [23:16], NSATT in [15] stays clear for group 0, and the INTID sits in [3:0]. GICD_SGIR is
// write-only.
//
// A core whose target bit is unpublished contributes nothing: bit `index` would be a bet on
// interface numbering.
void kickos_armv8a_gic_doorbell_send(uint32_t cores)
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
    // IHI 0048B.b specifies no ordering for SGI generation, so the far side's view of earlier
    // writes is the memory model's to order.
    __asm volatile("dsb ish" ::: "memory");
    *gicd32(GICD_SGIR) = ((list & 0xFFu) << 16) | static_cast<uint32_t>(GIC_SGI_DOORBELL);
}

// Write-1-to-clear over all eight source bits of this core's byte: an SGI's pending state is
// per target and per source core, and GICD_CPENDSGIR is banked to the accessing core.
void kickos_armv8a_gic_doorbell_clear(void)
{
    *gicd32(GICD_CPENDSGIR + (GIC_SGI_DOORBELL / 4) * 4)
        = 0xFFu << ((GIC_SGI_DOORBELL % 4) * 8);
}

// This core's banked enable word, doorbell bit excepted. Word 0 covers the SGI and PPI IDs,
// which are the only ones banked.
void kickos_armv8a_gic_doorbell_only(void)
{
    *gicd32(GICD_ICENABLER) = ~(1u << (GIC_SGI_DOORBELL % 32));
}
#endif

void kickos_armv8a_gic_clear_pending(int intid)
{
    *gicd32(GICD_ICPENDR + (intid / 32) * 4) = 1u << (intid % 32);
}

// --- Interrupt controller: the GIC behind the mask/unmask/clear triad -------
// The enable and pending registers are write-1-to-act, so no body read-modify-writes. Unmask
// writes the ENABLE last, so a half-applied sequence leaves the line masked.
//
// A line below GIC_BANKED_INTIDS reaches the calling core's own bank, so the same argument
// means a different interrupt on each core; a global line is taken by the core arch_irq_route
// recorded, or core 0 with no record.
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
    // Byte per INTID: a word index programs a different interrupt, and a 32-bit access is
    // also unaligned, which on Device memory faults.
    *gicd8(GICD_IPRIORITYR + static_cast<uintptr_t>(line)) = 0;
    RouteCell const* const cell = route_cell(line);
    if (cell != nullptr and cell->load() != 0)
    {
        // GICD_ITARGETSR already holds what arch_irq_route wrote, and this core may not be
        // the core it named.
        *gicd32(GICD_ISENABLER + (line / 32) * 4) = 1u << (line % 32);
        return;
    }
    if (line >= GIC_BANKED_INTIDS)
    {
        // No claim named a core: core zero's published interface number.
        *gicd8(GICD_ITARGETSR + static_cast<uintptr_t>(line)) = target_bit_of(0);
    }
    *gicd32(GICD_ISENABLER + (line / 32) * 4) = 1u << (line % 32);
}

// The line is masked while the target lands. A banked line has one copy per core and no target
// byte, so it is left alone: a claim of it claims the copy of the core the claimer is pinned to.
void arch_irq_route(int line, uint32_t core)
{
    RouteCell* const cell = route_cell(line);
    if (cell == nullptr or line >= kickos_gicv2.intid_count)
    {
        return;
    }
    if (core == KICKOS_IRQ_ROUTE_NONE)
    {
        *cell = 0;
        return;
    }
    uint8_t const bit = target_bit_of(core);
    if (bit == 0)
    {
        return; // no interface to write; the line stays unrouted
    }
    *gicd8(GICD_ITARGETSR + static_cast<uintptr_t>(line)) = bit;
    // LAST: the release store is what lets an unmask on another core read the write above.
    *cell = bit;
}

void arch_irq_clear_pending(int line)
{
    if (line < 0 or line >= kickos_gicv2.intid_count)
    {
        return;
    }
    kickos_armv8a_gic_clear_pending(line);
}

static_assert(not KICKOS_AMP_NODE or KICKOS_NUM_CORES > 1,
              "an AMP node under GICv2 drives more than one core, so its doorbell is up");

// The doorbell's SGI is the GIC's own, so no chip-file line carries it.
bool arch_irq_line_kernel_owned(int line)
{
#if KICKOS_NUM_CORES > 1
    return line == GIC_SGI_DOORBELL;
#else
    (void)line;
    return false;
#endif
}

// ISPENDR pends in the controller, so delivery takes the ordinary path. A banked line would pend
// on the caller's CPU interface, not its claim core's.
bool arch_irq_inject(int irq)
{
    if (irq < GIC_BANKED_INTIDS or irq >= kickos_gicv2.intid_count)
    {
        return false;
    }
    *gicd32(GICD_ISPENDR + (irq / 32) * 4) = 1u << (irq % 32);
    return true;
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
        kickos_doorbell_service();
    }
#endif
    else
    {
        kickos_isr_irq(static_cast<int>(intid));
    }
    // The whole value read: GICC_EOIR must carry back the CPUID that GICC_IAR[12:10] reported
    // for an SGI, and a PPI or SPI reports zero there.
    *gicc32(GICC_EOIR) = iar;
#if KICKOS_KERNEL_CORES > 1
    // After the EOI and outside the service body: this takes the kernel lock, which the
    // service body may not, answering an initiator that may hold it. The cell is the
    // authority, not the raise: the doorbell also carries rendezvous that owe no scheduler entry.
    if (intid == static_cast<uint32_t>(GIC_SGI_DOORBELL))
    {
        kickos_kernel_core_resched_if_owed();
    }
#endif
}

}
