// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// GICv3 backend for the arm64 family, behind gic.h. The chip's kickos_gicv3 (gicv3.h)
// supplies the register bases, the redistributor stride, the INTID count and the timer INTID.
//
// Every INTID this backend configures is Group 1. ICC_SGI1R_EL1 generates Group 1 only and a
// group mismatch drops an interrupt silently, so a timer PPI left in the group a reset chose
// yields a machine that boots, answers doorbells and never preempts. The acknowledge and
// end-of-interrupt registers are Group 1's for the same reason.

#include <kickos/arch/arch.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/chip_limits.h>

#include "gic.h"
#include "gicd.h"
#include "gicv3.h"
#include "smp_bringup.h"

#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    // VA - PA for the kernel's half, defined by every arm64 chip linker script.
    extern unsigned char __kickos_arm64_va_base[];

    void kpanic_refuse_unlocked(char const* line) __attribute__((noreturn));
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
        return reinterpret_cast<volatile uint32_t*>(dev_va(kickos_gicv3.dist_pa + off));
    }

    inline volatile uint8_t* gicd8(uintptr_t off)
    {
        return reinterpret_cast<volatile uint8_t*>(dev_va(kickos_gicv3.dist_pa + off));
    }

    inline volatile uint64_t* gicd64(uintptr_t off)
    {
        return reinterpret_cast<volatile uint64_t*>(dev_va(kickos_gicv3.dist_pa + off));
    }

    inline volatile uint32_t* gicr32(uintptr_t base, uintptr_t off)
    {
        return reinterpret_cast<volatile uint32_t*>(dev_va(base + off));
    }

    inline volatile uint8_t* gicr8(uintptr_t base, uintptr_t off)
    {
        return reinterpret_cast<volatile uint8_t*>(dev_va(base + off));
    }

    inline volatile uint64_t* gicr64(uintptr_t base, uintptr_t off)
    {
        return reinterpret_cast<volatile uint64_t*>(dev_va(base + off));
    }

    // Displacements beyond the shared distributor ones (gicd.h), IHI 0069H.b tables 12-25
    // (distributor), 12-27 and 12-29 (redistributor).
    constexpr uintptr_t GICD_IGROUPR = 0x0080;
    constexpr uintptr_t GICD_IROUTER = 0x6000;

    // RD_base holds the redistributor's own controls; SGI_base, the second 64 KB frame, holds
    // the state GICv2 banked in the distributor's first word.
    constexpr uintptr_t GICR_CTLR = 0x0000;
    constexpr uintptr_t GICR_TYPER = 0x0008;
    constexpr uintptr_t GICR_WAKER = 0x0014;
    constexpr uintptr_t GICR_SGI_FRAME = 0x10000;
    constexpr uintptr_t GICR_IGROUPR0 = GICR_SGI_FRAME + 0x0080;
    constexpr uintptr_t GICR_ISENABLER0 = GICR_SGI_FRAME + 0x0100;
    constexpr uintptr_t GICR_ICENABLER0 = GICR_SGI_FRAME + 0x0180;
    constexpr uintptr_t GICR_ISPENDR0 = GICR_SGI_FRAME + 0x0200;
    constexpr uintptr_t GICR_ICPENDR0 = GICR_SGI_FRAME + 0x0280;
    constexpr uintptr_t GICR_IPRIORITYR = GICR_SGI_FRAME + 0x0400;

    // The same write in both views this kernel can see: with a single Security state bit 4 is
    // ARE and bit 1 EnableGrp1; accessed Non-secure where two exist, bit 4 is ARE_NS and bit 1
    // EnableGrp1A. Bit 0 stays clear in both, so widening this to 0x13 enables Group 0.
    constexpr uint32_t GICD_CTLR_ARE = 1u << 4;
    constexpr uint32_t GICD_CTLR_GRP1 = 1u << 1;
    // Register write pending, one bit per frame, covering different registers. GICD_CTLR.RWP
    // (IHI 0069H.b 12.9.4) tracks GICD_ICENABLER<n>, GICD_CTLR[7:4], and a GICD_CTLR[2:0] group
    // enable falling from 1 to 0. GICR_CTLR.RWP (12.11.2) tracks GICR_ICENABLER0, the
    // GICR_CTLR.DPG bits, and EnableLPIs falling from 1 to 0.
    //
    // Nothing else is tracked: a set enable, a pending write, a priority, a group or a route
    // takes effect without one, so a wait after any of those would spin on a bit that never
    // rises. Add a wait where a disable is written and nowhere else.
    constexpr uint32_t GICD_CTLR_RWP = 1u << 31;
    constexpr uint32_t GICR_CTLR_RWP = 1u << 3;

    constexpr uint32_t GICR_WAKER_PROCESSOR_SLEEP = 1u << 1;
    constexpr uint32_t GICR_WAKER_CHILDREN_ASLEEP = 1u << 2;

    constexpr uint64_t GICR_TYPER_LAST = 1ull << 4;

    // ICC_IAR1_EL1 carries a 24-bit INTID, and 1020 to 1023 are the special values that name
    // no interrupt. None of them is acknowledged, so none owes an end of interrupt.
    constexpr uint32_t GIC_INTID_MASK = 0x00FFFFFFu;
    constexpr uint32_t GIC_INTID_SPECIAL = 1020;

    // ICC_CTLR_EL1.RSS, which says whether ICC_SGI1R_EL1.RS is anything but RES0.
    constexpr uint64_t ICC_CTLR_RSS = 1ull << 18;

    constexpr int SGI1R_AFF1_SHIFT = 16;
    constexpr int SGI1R_INTID_SHIFT = 24;
    constexpr int SGI1R_AFF2_SHIFT = 32;
    constexpr int SGI1R_RS_SHIFT = 44;
    constexpr int SGI1R_AFF3_SHIFT = 48;

    // One TargetList window addresses 16 affinity 0 values, and RS selects which window.
    constexpr uint32_t SGI_TARGETS_PER_WINDOW = 16;

    // Unlocked: an RWP wait can run inside the kernel lock's acquire poll.
    [[noreturn]] void refuse(char const* msg)
    {
        kpanic_refuse_unlocked(msg);
    }

    constexpr char NO_SRE[] = "gicv3: ICC_SRE_EL1.SRE reads back clear\n";
    constexpr char NO_FRAME[] = "gicv3: no redistributor frame carries this core's affinity\n";
    constexpr char STILL_ASLEEP[] = "gicv3: GICR_WAKER.ChildrenAsleep never cleared\n";
    constexpr char GICD_RWP_STUCK[] = "gicv3: GICD_CTLR.RWP never cleared\n";
    constexpr char GICR_RWP_STUCK[] = "gicv3: GICR_CTLR.RWP never cleared\n";
    constexpr char NO_RANGE[] = "gicv3: affinity 0 above 15 with ICC_CTLR_EL1.RSS clear\n";

    // Each core's RD_base, discovered rather than indexed: GICR_TYPER is the only thing that
    // says which core owns a frame. Written and read by that core alone.
    uintptr_t g_rd_base[KICKOS_NUM_CORES] = {};

    // Each core's affinity, packed Aff3:Aff2:Aff1:Aff0 one byte per level (GICR_TYPER's
    // Affinity_Value form), published by the core it names. Placed with the doorbell's cells:
    // a copy per image would leave every peer's row unseated.
    KICKOS_AMP_SHARED("affinity")
    kickos::Atomic<uint32_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>
        g_affinity[KICKOS_DOORBELL_CORES] = {};

    // Whether that affinity stands, published last: affinity zero is a real core, so the word
    // cannot say it, and a send reading an unpublished zero would target core zero instead.
    KICKOS_AMP_SHARED("affinity_seated")
    kickos::Atomic<uint8_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>
        g_affinity_seated[KICKOS_DOORBELL_CORES] = {};

    // Which core arch_irq_route named for a global line, biased by one so that unrouted is
    // zero. Written on the claiming core and read by an arch_irq_unmask that may run on
    // another, so the release store that seats a row publishes the GICD_IROUTER write ahead
    // of it.
    static_assert(KICKOS_MAX_IRQ > GIC_BANKED_INTIDS,
                  "a GIC with no global INTID leaves nothing to route");
    using RouteCell = kickos::Atomic<uint8_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>;
    RouteCell g_line_row[KICKOS_MAX_IRQ - GIC_BANKED_INTIDS] = {};

    // Null for a banked INTID and for one past the record.
    inline RouteCell* route_cell(int line)
    {
        if (line < GIC_BANKED_INTIDS or line >= KICKOS_MAX_IRQ)
        {
            return nullptr;
        }
        return &g_line_row[line - GIC_BANKED_INTIDS];
    }

    // Under an own-image AMP node the kernel's core index is always zero, not the machine's.
    inline uint32_t route_row(uint32_t core)
    {
#if KICKOS_AMP_OWN_IMAGE
        (void)core;
        return arch_doorbell_core();
#else
        return core;
#endif
    }

    inline uint32_t affinity_packed(uint64_t mpidr)
    {
        uint32_t const aff0 = static_cast<uint32_t>(mpidr) & 0xFFu;
        uint32_t const aff1 = static_cast<uint32_t>(mpidr >> 8) & 0xFFu;
        uint32_t const aff2 = static_cast<uint32_t>(mpidr >> 16) & 0xFFu;
        uint32_t const aff3 = static_cast<uint32_t>(mpidr >> 32) & 0xFFu;
        return (aff3 << 24) | (aff2 << 16) | (aff1 << 8) | aff0;
    }

    inline uint64_t mpidr_now(void)
    {
        uint64_t mpidr = 0;
        __asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        return mpidr;
    }

    // GICD_IROUTER takes MPIDR_EL1's packing, IRM (bit 31) clear: the one core named.
    inline uint64_t router_value(uint32_t packed)
    {
        uint64_t const aff3 = static_cast<uint64_t>(packed >> 24) & 0xFFull;
        return (aff3 << 32) | static_cast<uint64_t>(packed & 0x00FFFFFFu);
    }

    // The GICR_* displacements above already carry the SGI frame.
    inline uintptr_t my_rd_base(void)
    {
        return g_rd_base[arch_cpu_id()];
    }

    // A disable is not in effect until RWP reads zero, so a mask that returns before that is
    // not exclusion. The bit is tested before the deadline is computed: folding the two tests
    // into one loop would put a counter read on every mask.
    void wait_gicd_rwp(void)
    {
        if ((*gicd32(GICD_CTLR) & GICD_CTLR_RWP) == 0)
        {
            return;
        }
        uint64_t const deadline = arch_clock_now() + kickos::ARM64_BRINGUP_WAIT_NS;
        while ((*gicd32(GICD_CTLR) & GICD_CTLR_RWP) != 0)
        {
            if (arch_clock_now() > deadline)
            {
                refuse(GICD_RWP_STUCK);
            }
            __asm volatile("yield" ::: "memory");
        }
    }

    // The calling core's own redistributor: GICR_CTLR.RWP is per frame, so it reports writes
    // made here and no peer's.
    void wait_gicr_rwp(uintptr_t base)
    {
        if ((*gicr32(base, GICR_CTLR) & GICR_CTLR_RWP) == 0)
        {
            return;
        }
        uint64_t const deadline = arch_clock_now() + kickos::ARM64_BRINGUP_WAIT_NS;
        while ((*gicr32(base, GICR_CTLR) & GICR_CTLR_RWP) != 0)
        {
            if (arch_clock_now() > deadline)
            {
                refuse(GICR_RWP_STUCK);
            }
            __asm volatile("yield" ::: "memory");
        }
    }

    // First of all: while SRE reads clear, every other ICC_* access at EL1 traps. The field
    // can be RAO or RAZ/WI, hence the read back.
    void enable_system_registers(void)
    {
        uint64_t sre = 0;
        __asm volatile("mrs %0, icc_sre_el1" : "=r"(sre));
        sre |= 1ull;
        __asm volatile("msr icc_sre_el1, %0" ::"r"(sre));
        __asm volatile("isb" ::: "memory");
        __asm volatile("mrs %0, icc_sre_el1" : "=r"(sre));
        if ((sre & 1ull) == 0)
        {
            refuse(NO_SRE);
        }
    }

    // The frame whose GICR_TYPER names this core. The walk stops at the Last bit and is bounded
    // by the part's frame count, so a series that never sets Last cannot run off the reserved
    // window. Not bounded on KICKOS_NUM_CORES: a core handed over at EL1 on a die whose other
    // frames this image never drove still has to reach its own.
    uintptr_t find_my_redistributor(uint32_t packed)
    {
        uintptr_t base = kickos_gicv3.rdist_pa;
        for (int seen = 0; seen < kickos_gicv3.rdist_count; seen++)
        {
            uint64_t const typer = *gicr64(base, GICR_TYPER);
            if (static_cast<uint32_t>(typer >> 32) == packed)
            {
                return base;
            }
            if ((typer & GICR_TYPER_LAST) != 0)
            {
                break;
            }
            base += kickos_gicv3.rdist_stride;
        }
        refuse(NO_FRAME);
    }

    // Until ChildrenAsleep clears, this redistributor forwards nothing to its core.
    void wake_redistributor(uintptr_t base)
    {
        uint32_t waker = *gicr32(base, GICR_WAKER);
        *gicr32(base, GICR_WAKER) = waker & ~GICR_WAKER_PROCESSOR_SLEEP;
        uint64_t const deadline = arch_clock_now() + kickos::ARM64_BRINGUP_WAIT_NS;
        while ((*gicr32(base, GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) != 0)
        {
            if (arch_clock_now() > deadline)
            {
                refuse(STILL_ASLEEP);
            }
            __asm volatile("yield" ::: "memory");
        }
    }

#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
    // A core-index mask is what crosses gic.h, at the partition's width: an own-image AMP node
    // names cores its own image does not drive.
    static_assert(KICKOS_DOORBELL_CORES <= 32, "a core-index mask is 32 bits wide");

    // The doorbell's INTID. ICC_SGI1R_EL1 carries a 4-bit INTID.
    constexpr int GIC_SGI_DOORBELL = 0;

    // One write per affinity-and-range window: TargetList is 16 bits of affinity 0 within the
    // cluster Aff3.Aff2.Aff1, and RS picks which group of 16. IRM stays clear: an exact subset.
    void sgi1r_write(uint32_t cluster, uint32_t rs, uint32_t targets)
    {
        uint64_t const aff1 = static_cast<uint64_t>((cluster >> 8) & 0xFFu);
        uint64_t const aff2 = static_cast<uint64_t>((cluster >> 16) & 0xFFu);
        uint64_t const aff3 = static_cast<uint64_t>((cluster >> 24) & 0xFFu);
        uint64_t const value = (aff3 << SGI1R_AFF3_SHIFT) | (static_cast<uint64_t>(rs) << SGI1R_RS_SHIFT)
                               | (aff2 << SGI1R_AFF2_SHIFT)
                               | (static_cast<uint64_t>(GIC_SGI_DOORBELL) << SGI1R_INTID_SHIFT)
                               | (aff1 << SGI1R_AFF1_SHIFT) | static_cast<uint64_t>(targets);
        __asm volatile("msr icc_sgi1r_el1, %0" ::"r"(value));
    }
#endif
}

extern "C"
{

// The distributor's shared half. Affinity routing goes on before any line is configured: it
// makes GICD_IROUTER the routing register and the distributor's first word RES0.
//
// ARE moves 0 to 1 with every group enable already clear; merging the enable into that write
// is UNPREDICTABLE.
void kickos_armv8a_gic_dist_init(void)
{
    *gicd32(GICD_CTLR) = 0;
    wait_gicd_rwp();
    *gicd32(GICD_CTLR) = GICD_CTLR_ARE;
    wait_gicd_rwp();

    // Every shared line starts masked and in Group 1: an SPI left in Group 0 is never
    // acknowledged by ICC_IAR1_EL1. The first word is the core's own bank, so the sweep starts
    // at the first shared ID.
    for (int intid = GIC_BANKED_INTIDS; intid < kickos_gicv3.intid_count; intid += 32)
    {
        *gicd32(GICD_ICENABLER + (intid / 32) * 4) = 0xFFFFFFFFu;
        *gicd32(GICD_ICPENDR + (intid / 32) * 4) = 0xFFFFFFFFu;
        *gicd32(GICD_IGROUPR + (intid / 32) * 4) = 0xFFFFFFFFu;
    }
    // One wait for the whole sweep, before the group enable makes any line deliverable.
    wait_gicd_rwp();

    *gicd32(GICD_CTLR) = GICD_CTLR_ARE | GICD_CTLR_GRP1;
    wait_gicd_rwp();
}

// This core's hardware edge alone: the ICC_* registers and its own redistributor. Idempotent:
// a secondary runs it in the park loop and again on the way to the scheduler.
void kickos_armv8a_gic_percore_init(void)
{
    enable_system_registers();
    // The interface off while the redistributor is configured under it.
    __asm volatile("msr icc_igrpen1_el1, %0" ::"r"(uint64_t(0)));
    __asm volatile("isb" ::: "memory");

    uint64_t const mpidr = mpidr_now();
    uint32_t const packed = affinity_packed(mpidr);
    uintptr_t const base = find_my_redistributor(packed);
    g_rd_base[arch_cpu_id()] = base;
    wake_redistributor(base);

    // The sweep covers the SGI IDs, so a doorbell must enable its own INTID after this.
    *gicr32(base, GICR_ICENABLER0) = 0xFFFFFFFFu;
    wait_gicr_rwp(base);
    *gicr32(base, GICR_ICPENDR0) = 0xFFFFFFFFu;
    // Group 1 for every banked INTID, the timer PPI and the doorbell SGI among them.
    *gicr32(base, GICR_IGROUPR0) = 0xFFFFFFFFu;

    // The reset PMR of 0 blocks priority 0, where every line here sits. ICC_CTLR_EL1 written
    // whole leaves EOImode clear, so one end of interrupt both drops the priority and
    // deactivates.
    __asm volatile("msr icc_pmr_el1, %0" ::"r"(uint64_t(0xF0)));
    __asm volatile("msr icc_bpr1_el1, %0" ::"r"(uint64_t(0)));
    __asm volatile("msr icc_ctlr_el1, %0" ::"r"(uint64_t(0)));
    __asm volatile("msr icc_igrpen1_el1, %0" ::"r"(uint64_t(1)));
    __asm volatile("isb" ::: "memory");

    // Banked: no route is owed.
    int const timer = kickos_gicv3.timer_intid;
    *gicr8(base, GICR_IPRIORITYR + static_cast<uintptr_t>(timer)) = 0;
    *gicr32(base, GICR_ISENABLER0) = 1u << (timer % 32);

#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
    // GICR_ICPENDR0 carries one bit per INTID with no source identity, so this clears a raise
    // latched before this core owned an interface, whoever made it.
    *gicr32(base, GICR_ICPENDR0) = 1u << (GIC_SGI_DOORBELL % 32);
    *gicr8(base, GICR_IPRIORITYR + static_cast<uintptr_t>(GIC_SGI_DOORBELL)) = 0;
    *gicr32(base, GICR_ISENABLER0) = 1u << (GIC_SGI_DOORBELL % 32);

    // A core beyond the first 16 affinity 0 values is unreachable where the interface cannot
    // select ranges. Read on this core, which decides it for the senders too: RSS is a
    // property of the one cluster.
    uint64_t icc_ctlr = 0;
    __asm volatile("mrs %0, icc_ctlr_el1" : "=r"(icc_ctlr));
    if ((packed & 0xFFu) >= SGI_TARGETS_PER_WINDOW and (icc_ctlr & ICC_CTLR_RSS) == 0)
    {
        refuse(NO_RANGE);
    }
#endif

    // Published last, so a sender that finds the seat reaches a live interface.
    g_affinity[arch_doorbell_core()] = packed;
    g_affinity_seated[arch_doorbell_core()] = 1u;
}

#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
// One write per affinity-and-range window: GICv3 has no target list spanning clusters.
//
// A core whose affinity is unpublished contributes to no window: a zero in the array is core
// zero's affinity, not an absence.
void kickos_armv8a_gic_doorbell_send(uint32_t cores)
{
    // The ring is the authority and this raise is a hint: a core whose affinity is unpublished
    // cannot be targeted, so only its notice is deferred, that peer draining what it was sent
    // before it waits on a doorbell of any kind. A credit return is not a hint; deferring one
    // is safe only because window_init services unconditionally before that peer waits.
    //
    // The seating flag only goes unseated to seated, so a load reading seated is never stale.
    // A load reading unseated may be, and skipping on a stale one strands a publication with
    // no notice and no later scan, so that decision alone is made behind a full barrier,
    // pairing with the one the peer runs between seating and draining.
    uint32_t pending = 0;
    for (uint32_t index = 0; index < KICKOS_DOORBELL_CORES; index++)
    {
        if ((cores & (1u << index)) == 0)
        {
            continue;
        }
        if (g_affinity_seated[index].load() != 0)
        {
            pending |= 1u << index;
            continue;
        }
        arch_ipi_fence();
        if (g_affinity_seated[index].load() != 0)
        {
            pending |= 1u << index;
        }
    }
    if (pending == 0)
    {
        return;
    }
    // The architecture orders an SGI against nothing.
    __asm volatile("dsb ish" ::: "memory");

    while (pending != 0)
    {
        uint32_t lead = 0;
        while ((pending & (1u << lead)) == 0)
        {
            lead++;
        }
        // The lead leaves `pending` here, not in the sweep below: a bound that stopped agreeing
        // with the mask would else leave the lead bit set and spin this core masked.
        pending &= ~(1u << lead);
        uint32_t const packed = g_affinity[lead].load();
        uint32_t const cluster = packed & 0xFFFFFF00u;
        uint32_t const rs = (packed & 0xFFu) / SGI_TARGETS_PER_WINDOW;
        uint32_t targets = 1u << ((packed & 0xFFu) % SGI_TARGETS_PER_WINDOW);
        // The partition's width, not this image's: an own-image AMP node drives one core while
        // naming peers on others.
        for (uint32_t index = lead + 1u; index < KICKOS_DOORBELL_CORES; index++)
        {
            if ((pending & (1u << index)) == 0)
            {
                continue;
            }
            uint32_t const other = g_affinity[index].load();
            if ((other & 0xFFFFFF00u) != cluster
                or (other & 0xFFu) / SGI_TARGETS_PER_WINDOW != rs)
            {
                continue;
            }
            targets |= 1u << ((other & 0xFFu) % SGI_TARGETS_PER_WINDOW);
            pending &= ~(1u << index);
        }
        sgi1r_write(cluster, rs, targets);
    }
    // The raise is a system register write: not in effect until the context synchronises.
    __asm volatile("isb" ::: "memory");
}

// A GICv3 SGI has one pending bit per target, so one store drops the whole doorbell.
void kickos_armv8a_gic_doorbell_clear(void)
{
    *gicr32(my_rd_base(), GICR_ICPENDR0) = 1u << (GIC_SGI_DOORBELL % 32);
}

// This core's banked enable word, doorbell bit excepted.
void kickos_armv8a_gic_doorbell_only(void)
{
    uintptr_t const base = my_rd_base();
    *gicr32(base, GICR_ICENABLER0) = ~(1u << (GIC_SGI_DOORBELL % 32));
    // The caller parks with interrupts open straight after this: a still-pending disable
    // would let the timer PPI into a core that reaches no scheduler.
    wait_gicr_rwp(base);
}
#endif

void kickos_armv8a_gic_clear_pending(int intid)
{
    if (intid < GIC_BANKED_INTIDS)
    {
        *gicr32(my_rd_base(), GICR_ICPENDR0) = 1u << (intid % 32);
        return;
    }
    *gicd32(GICD_ICPENDR + (intid / 32) * 4) = 1u << (intid % 32);
}

// --- Interrupt controller: the GIC behind the mask/unmask/clear triad -------
// The enable and pending registers are write-1-to-act, so no body read-modify-writes. Unmask
// writes the ENABLE last, so a half-applied sequence leaves the line masked.
//
// Only mask waits: RWP tracks a cleared enable and nothing else, and without the wait the seam
// returns with delivery still possible.
//
// A line below GIC_BANKED_INTIDS is held in the calling core's own redistributor, so the same
// argument means a different interrupt on each core; a global line is taken by the core
// arch_irq_route recorded, or by whichever core unmasks it with no record.
void arch_irq_mask(int line)
{
    if (line < 0 or line >= kickos_gicv3.intid_count)
    {
        return;
    }
    if (line < GIC_BANKED_INTIDS)
    {
        uintptr_t const base = my_rd_base();
        *gicr32(base, GICR_ICENABLER0) = 1u << (line % 32);
        wait_gicr_rwp(base);
        return;
    }
    *gicd32(GICD_ICENABLER + (line / 32) * 4) = 1u << (line % 32);
    wait_gicd_rwp();
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= kickos_gicv3.intid_count)
    {
        return;
    }
    if (line < GIC_BANKED_INTIDS)
    {
        // Byte per INTID: a word index programs a different interrupt, and a 32-bit access is
        // also unaligned, which on Device memory faults.
        *gicr8(my_rd_base(), GICR_IPRIORITYR + static_cast<uintptr_t>(line)) = 0;
        *gicr32(my_rd_base(), GICR_ISENABLER0) = 1u << (line % 32);
        return;
    }
    *gicd8(GICD_IPRIORITYR + static_cast<uintptr_t>(line)) = 0;
    RouteCell const* const cell = route_cell(line);
    if (cell != nullptr and cell->load() != 0)
    {
        // GICD_IROUTER already holds what arch_irq_route wrote, and this core may not be the
        // core it named.
        *gicd32(GICD_ISENABLER + (line / 32) * 4) = 1u << (line % 32);
        return;
    }
    // No claim named a core, so the line follows this one by its published affinity. Unlike
    // GICv2, where an empty target byte names nobody, affinity zero is a real core: with none
    // published the line is left masked rather than routed to a guess.
    if (g_affinity_seated[arch_doorbell_core()].load() == 0)
    {
        return;
    }
    *gicd64(GICD_IROUTER + static_cast<uintptr_t>(line) * 8) =
        router_value(g_affinity[arch_doorbell_core()].load());
    *gicd32(GICD_ISENABLER + (line / 32) * 4) = 1u << (line % 32);
}

// The line is masked while the route lands. A banked line lives in each core's redistributor
// and has no router, so it is left alone: a claim of it claims the copy of the core the
// claimer is pinned to.
void arch_irq_route(int line, uint32_t core)
{
    RouteCell* const cell = route_cell(line);
    if (cell == nullptr or line >= kickos_gicv3.intid_count)
    {
        return;
    }
    if (core == KICKOS_IRQ_ROUTE_NONE)
    {
        *cell = 0;
        return;
    }
    uint32_t const row = route_row(core);
    if (row >= KICKOS_DOORBELL_CORES or g_affinity_seated[row].load() == 0)
    {
        return; // no affinity to write; the line stays unrouted
    }
    *gicd64(GICD_IROUTER + static_cast<uintptr_t>(line) * 8) =
        router_value(g_affinity[row].load());
    // LAST: the release store is what lets an unmask on another core read the write above.
    *cell = static_cast<uint8_t>(row + 1u);
}

void arch_irq_clear_pending(int line)
{
    if (line < 0 or line >= kickos_gicv3.intid_count)
    {
        return;
    }
    kickos_armv8a_gic_clear_pending(line);
}

// The doorbell's SGI and the timer PPI are the GIC's own; no chip-file line carries them.
bool arch_irq_line_kernel_owned(int line)
{
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
    if (line == GIC_SGI_DOORBELL)
    {
        return true;
    }
#endif
    return line == kickos_gicv3.timer_intid;
}

// The set-pending registers pend in the controller, so delivery takes the ordinary path. A
// banked line would pend on the caller's redistributor, not its claim core's.
bool arch_irq_inject(int irq)
{
    if (irq < GIC_BANKED_INTIDS or irq >= kickos_gicv3.intid_count)
    {
        return false;
    }
    *gicd32(GICD_ISPENDR + (irq / 32) * 4) = 1u << (irq % 32);
    return true;
}

// One interrupt per entry: the GIC signals again for anything still pending.
void kickos_armv8a_gic_dispatch(void)
{
    uint64_t iar = 0;
    __asm volatile("mrs %0, icc_iar1_el1" : "=r"(iar));
    uint32_t const intid = static_cast<uint32_t>(iar) & GIC_INTID_MASK;
    if (intid >= GIC_INTID_SPECIAL)
    {
        return; // no end of interrupt is owed for a special INTID
    }
    if (intid == static_cast<uint32_t>(kickos_gicv3.timer_intid))
    {
        // The output is LEVEL, and kickos_isr_timer's re-arm or disarm is what lowers it;
        // an end of interrupt alone would re-enter here forever.
        kickos_isr_timer();
    }
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
    else if (intid == static_cast<uint32_t>(GIC_SGI_DOORBELL))
    {
        // SGIs are edge-triggered, and the acknowledge above cleared the pending state.
        kickos_doorbell_service();
    }
#endif
    else
    {
        kickos_isr_irq(static_cast<int>(intid));
    }
    // The INTID alone: a GICv3 acknowledge carries no SGI source field.
    __asm volatile("msr icc_eoir1_el1, %0" ::"r"(static_cast<uint64_t>(intid)));
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
