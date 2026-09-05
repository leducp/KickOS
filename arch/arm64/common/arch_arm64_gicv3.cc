// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// GICv3 backend for the arm64 family: the half ARM architects, behind gic.h. The chip's
// kickos_gicv3 (gicv3.h) supplies the distributor and redistributor bases, the stride from one
// core's redistributor to the next, the INTID count and the INTID the EL1 physical timer
// asserts.
//
// EVERY INTID THIS BACKEND CONFIGURES IS GROUP 1. ICC_SGI1R_EL1 generates Group 1 only and a
// group mismatch drops an interrupt silently, so a timer PPI left in the group a reset chose
// yields a machine that boots, answers doorbells and never preempts. The acknowledge and
// end-of-interrupt registers are Group 1's for the same reason.

#include <kickos/arch/arch.h>
#include <kickos/arch/doorbell_cells.h>

#include "gic.h"
#include "gicv3.h"
#include "smp_bringup.h"

#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    // VA - PA for the kernel's half, defined by every arm64 chip linker script.
    extern unsigned char __kickos_arm64_va_base[];

    void kfault_terminate(void) __attribute__((noreturn));
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

    // Displacements, IHI 0069H.b tables 12-25 (distributor), 12-27 and 12-29 (redistributor).
    constexpr uintptr_t GICD_CTLR = 0x0000;
    constexpr uintptr_t GICD_IGROUPR = 0x0080;
    constexpr uintptr_t GICD_ISENABLER = 0x0100;
    constexpr uintptr_t GICD_ICENABLER = 0x0180;
    constexpr uintptr_t GICD_ISPENDR = 0x0200;
    constexpr uintptr_t GICD_ICPENDR = 0x0280;
    constexpr uintptr_t GICD_IPRIORITYR = 0x0400;
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

    // THE SAME WRITE IN BOTH VIEWS THIS KERNEL CAN SEE: with a single Security state bit 4 is
    // ARE and bit 1 EnableGrp1; accessed Non-secure where two exist, bit 4 is ARE_NS and bit 1
    // EnableGrp1A. Bit 0 stays clear in both, so widening this to 0x13 enables Group 0.
    constexpr uint32_t GICD_CTLR_ARE = 1u << 4;
    constexpr uint32_t GICD_CTLR_GRP1 = 1u << 1;
    // REGISTER WRITE PENDING, one bit per frame, covering different registers.
    // GICD_CTLR.RWP (IHI 0069H.b 12.9.4) tracks GICD_ICENABLER<n>,
    // GICD_CTLR[7:4], and a GICD_CTLR[2:0] group enable falling from 1 to 0. GICR_CTLR.RWP
    // (12.11.2) tracks GICR_ICENABLER0, the GICR_CTLR.DPG bits, and EnableLPIs falling from 1
    // to 0.
    //
    // NOTHING ELSE IS TRACKED. A SET enable, a pending write, a priority, a group or a route
    // takes effect without one, so a wait after any of those would spin on a bit that never
    // rises. Add a wait where a DISABLE is written and nowhere else.
    constexpr uint32_t GICD_CTLR_RWP = 1u << 31;
    constexpr uint32_t GICR_CTLR_RWP = 1u << 3;

    constexpr uint32_t GICR_WAKER_PROCESSOR_SLEEP = 1u << 1;
    constexpr uint32_t GICR_WAKER_CHILDREN_ASLEEP = 1u << 2;

    constexpr uint64_t GICR_TYPER_LAST = 1ull << 4;

    // THE KIND, as a value range rather than a separate field (roadmap.md's `(line, kind)`).
    // Below 32 an INTID is the calling core's own, and with affinity routing on it is reached
    // in that core's REDISTRIBUTOR rather than in the distributor, whose first word is RES0
    // there. At or above 32 an interrupt is global and reaches no core until GICD_IROUTER
    // names one. Every arch_irq_* body branches on this boundary and nothing else does.
    constexpr int GIC_BANKED_INTIDS = 32;

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

    [[noreturn]] void refuse(char const* msg, size_t n)
    {
        arch_console_write(msg, n);
        kfault_terminate();
    }

    constexpr char NO_SRE[] = "gicv3: ICC_SRE_EL1.SRE reads back clear\n";
    constexpr char NO_FRAME[] = "gicv3: no redistributor frame carries this core's affinity\n";
    constexpr char STILL_ASLEEP[] = "gicv3: GICR_WAKER.ChildrenAsleep never cleared\n";
    constexpr char GICD_RWP_STUCK[] = "gicv3: GICD_CTLR.RWP never cleared\n";
    constexpr char GICR_RWP_STUCK[] = "gicv3: GICR_CTLR.RWP never cleared\n";
    constexpr char NO_RANGE[] = "gicv3: affinity 0 above 15 with ICC_CTLR_EL1.RSS clear\n";

    // Each core's RD_base, DISCOVERED rather than indexed: the redistributor frames are
    // ordered by the implementation and GICR_TYPER is the only thing that says which core owns
    // one. Written by that core alone and read by that core alone, the banked registers being
    // the calling core's by definition.
    uintptr_t g_rd_base[KICKOS_NUM_CORES] = {};

    // Each core's affinity, packed Aff3:Aff2:Aff1:Aff0 one byte per level, which is
    // GICR_TYPER's Affinity_Value form. Published by the core it names.
    //
    // Both nodes write it under one image per node, so it is placed with the doorbell's cells:
    // a copy per image leaves every peer's row unseated, and the send below SKIPS an unseated
    // core.
    KICKOS_AMP_SHARED("affinity")
    kickos::Atomic<uint32_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>
        g_affinity[KICKOS_DOORBELL_CORES] = {};

    // WHETHER THAT AFFINITY STANDS, published last. Affinity zero is a real core rather than
    // an absence, so unlike a GICv2 target BIT the affinity word cannot say this itself, and a
    // send that read an unpublished zero would target core zero twice and the intended core
    // never.
    KICKOS_AMP_SHARED("affinity_seated")
    kickos::Atomic<uint8_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>
        g_affinity_seated[KICKOS_DOORBELL_CORES] = {};

#if defined(KICKOS_ENABLE_SELFTEST) && (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
    // Raises this core skipped because the target was unseated, per target: the only place a
    // peer that never started can be counted from.
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_deferred[KICKOS_DOORBELL_CORES] = {};
#endif

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

    // GICD_IROUTER takes MPIDR_EL1's packing, with IRM in bit 31 left clear so the line goes
    // to the one core named rather than to any core.
    inline uint64_t router_value(uint32_t packed)
    {
        uint64_t const aff3 = static_cast<uint64_t>(packed >> 24) & 0xFFull;
        return (aff3 << 32) | static_cast<uint64_t>(packed & 0x00FFFFFFu);
    }

    // This core's own redistributor, which is where its banked interrupt state lives. The
    // GICR_* displacements above already carry the SGI frame.
    inline uintptr_t my_rd_base(void)
    {
        return g_rd_base[arch_cpu_id()];
    }

    // A DISABLE IS NOT IN EFFECT UNTIL RWP READS ZERO, the propagation being asynchronous, so
    // a mask that returns before that is not exclusion. What each RWP covers is at
    // GICD_CTLR_RWP above.
    //
    // THE BIT IS TESTED BEFORE THE DEADLINE IS COMPUTED, so a controller that completed the
    // write synchronously costs one Device read and no counter read. Folding the two tests
    // into one loop puts a counter read on every mask.
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
                refuse(GICD_RWP_STUCK, sizeof(GICD_RWP_STUCK) - 1);
            }
            __asm volatile("yield" ::: "memory");
        }
    }

    // THE CALLING CORE'S OWN REDISTRIBUTOR: GICR_CTLR.RWP is per frame, so it reports writes
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
                refuse(GICR_RWP_STUCK, sizeof(GICR_RWP_STUCK) - 1);
            }
            __asm volatile("yield" ::: "memory");
        }
    }

    // ICC_SRE_EL1.SRE FIRST OF ALL: while it reads clear, every other ICC_* access at EL1
    // traps to EL1, so a backend that set it late would fault on the register it set it with.
    // The field can be RAO or RAZ/WI, hence the read back.
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
            refuse(NO_SRE, sizeof(NO_SRE) - 1);
        }
    }

    // The frame whose GICR_TYPER names this core, found by walking the contiguous series the
    // chip declares. The walk stops at the Last bit and is bounded by the PART's frame count
    // either way, so a series that never sets Last cannot run off the end of the window the
    // grant model reserved. BOUNDED ON THE PART AND NOT ON KICKOS_NUM_CORES: a core handed over
    // at EL1 on a die whose other frames this image never drove still has to reach its own.
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
        refuse(NO_FRAME, sizeof(NO_FRAME) - 1);
    }

    // ProcessorSleep clear and ChildrenAsleep polled down: until it is, this redistributor
    // forwards nothing to its core. BOUNDED like every other wait in this family, so a
    // secondary whose redistributor never wakes refuses instead of hanging.
    void wake_redistributor(uintptr_t base)
    {
        uint32_t waker = *gicr32(base, GICR_WAKER);
        *gicr32(base, GICR_WAKER) = waker & ~GICR_WAKER_PROCESSOR_SLEEP;
        uint64_t const deadline = arch_clock_now() + kickos::ARM64_BRINGUP_WAIT_NS;
        while ((*gicr32(base, GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) != 0)
        {
            if (arch_clock_now() > deadline)
            {
                refuse(STILL_ASLEEP, sizeof(STILL_ASLEEP) - 1);
            }
            __asm volatile("yield" ::: "memory");
        }
    }

#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
    // A core index mask is what crosses gic.h, so a target list of 32 is the widest this
    // backend is ever handed, at the PARTITION's width: an own-image AMP node is handed a mask
    // naming cores its own image does not drive.
    static_assert(KICKOS_DOORBELL_CORES <= 32, "a core-index mask is 32 bits wide");

    // The doorbell's INTID. ICC_SGI1R_EL1 carries a 4-bit INTID.
    constexpr int GIC_SGI_DOORBELL = 0;
    static_assert(GIC_SGI_DOORBELL >= 0 and GIC_SGI_DOORBELL <= 15,
                  "ICC_SGI1R_EL1 carries a 4-bit INTID");

    // One write per affinity-and-range window: TargetList is 16 bits of affinity 0 within the
    // cluster Aff3.Aff2.Aff1, and RS picks which group of 16. IRM stays clear, the caller
    // owing an exact subset rather than every core but this one.
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

// The distributor's SHARED half, one write set for the machine. Affinity routing goes on before
// any line is configured: it is what makes GICD_IROUTER the routing register and the
// distributor's first word RES0.
//
// ARE MOVES 0 TO 1 WITH EVERY GROUP ENABLE ALREADY CLEAR, which is the only ordering the
// architecture makes predictable. Merging the enable into that write is UNPREDICTABLE.
void kickos_armv8a_gic_dist_init(void)
{
    *gicd32(GICD_CTLR) = 0;
    wait_gicd_rwp();
    *gicd32(GICD_CTLR) = GICD_CTLR_ARE;
    wait_gicd_rwp();

    // arch.h's reset contract over the shared lines, and Group 1 over the same range: an SPI
    // left in Group 0 is never acknowledged by ICC_IAR1_EL1. The first word is the running
    // core's own bank and belongs with its redistributor, so the sweep starts at the first
    // shared ID.
    for (int intid = GIC_BANKED_INTIDS; intid < kickos_gicv3.intid_count; intid += 32)
    {
        *gicd32(GICD_ICENABLER + (intid / 32) * 4) = 0xFFFFFFFFu;
        *gicd32(GICD_ICPENDR + (intid / 32) * 4) = 0xFFFFFFFFu;
        *gicd32(GICD_IGROUPR + (intid / 32) * 4) = 0xFFFFFFFFu;
    }
    // One wait for the whole sweep: RWP reports every earlier write, so the disables are in
    // effect before the group enable below makes any of those lines deliverable.
    wait_gicd_rwp();

    *gicd32(GICD_CTLR) = GICD_CTLR_ARE | GICD_CTLR_GRP1;
    wait_gicd_rwp();
}

// This core's hardware edge alone: its CPU interface is the ICC_* registers and its banked
// state is in its own redistributor. IDEMPOTENT, a secondary running it once in the park loop
// and again on the way to the scheduler.
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

    // arch.h's reset contract over this core's banked lines. The sweep covers the SGI IDs, so
    // a cross-core doorbell must enable its own INTID after this has run.
    *gicr32(base, GICR_ICENABLER0) = 0xFFFFFFFFu;
    wait_gicr_rwp(base);
    *gicr32(base, GICR_ICPENDR0) = 0xFFFFFFFFu;
    // GROUP 1 FOR EVERY BANKED INTID, the timer PPI and the doorbell SGI among them.
    *gicr32(base, GICR_IGROUPR0) = 0xFFFFFFFFu;

    // PMR wide open at 0xF0: everything here sits at priority 0, and the reset PMR of 0 blocks
    // all of it. BPR1 at 0 groups no priorities, and ICC_CTLR_EL1 written whole leaves EOImode
    // clear, so one end-of-interrupt both drops the priority and deactivates.
    __asm volatile("msr icc_pmr_el1, %0" ::"r"(uint64_t(0xF0)));
    __asm volatile("msr icc_bpr1_el1, %0" ::"r"(uint64_t(0)));
    __asm volatile("msr icc_ctlr_el1, %0" ::"r"(uint64_t(0)));
    __asm volatile("msr icc_igrpen1_el1, %0" ::"r"(uint64_t(1)));
    __asm volatile("isb" ::: "memory");

    // Banked, so no route is owed: the timer PPI named from this core is this core's own.
    int const timer = kickos_gicv3.timer_intid;
    *gicr8(base, GICR_IPRIORITYR + static_cast<uintptr_t>(timer)) = 0;
    *gicr32(base, GICR_ISENABLER0) = 1u << (timer % 32);

#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
    // GICR_ICPENDR0 carries one bit per INTID with no source identity, so this clears a raise
    // latched before this core owned an interface, whoever made it.
    *gicr32(base, GICR_ICPENDR0) = 1u << (GIC_SGI_DOORBELL % 32);
    *gicr8(base, GICR_IPRIORITYR + static_cast<uintptr_t>(GIC_SGI_DOORBELL)) = 0;
    *gicr32(base, GICR_ISENABLER0) = 1u << (GIC_SGI_DOORBELL % 32);

    // A TARGET LIST ADDRESSES 16 AFFINITY 0 VALUES AND RS PICKS THE WINDOW, so a core beyond
    // the first window is unreachable where the interface cannot select ranges. Read on this
    // core for this core, which decides the question for the senders too: arch_cpu_id already
    // claims one cluster of symmetric cores, and RSS is a property of that cluster.
    uint64_t icc_ctlr = 0;
    __asm volatile("mrs %0, icc_ctlr_el1" : "=r"(icc_ctlr));
    if ((packed & 0xFFu) >= SGI_TARGETS_PER_WINDOW and (icc_ctlr & ICC_CTLR_RSS) == 0)
    {
        refuse(NO_RANGE, sizeof(NO_RANGE) - 1);
    }
#endif

    // Published last, so a sender that finds the seat reaches a live interface.
    g_affinity[arch_doorbell_core()] = packed;
    g_affinity_seated[arch_doorbell_core()] = 1u;
}

#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
// ONE WRITE PER AFFINITY-AND-RANGE WINDOW: GICv3 has no target list spanning clusters, so how
// many writes a send costs is a property of the machine's topology rather than of the mask.
//
// A core whose affinity is unpublished contributes to no window: a zero read out of the array
// is core zero's affinity rather than an absence.
#if defined(KICKOS_ENABLE_SELFTEST) && (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
uint32_t kickos_armv8a_gic_seat_set(uint32_t core, uint32_t seated)
{
    if (core >= KICKOS_DOORBELL_CORES)
    {
        return 0u;
    }
    uint32_t const was = g_affinity_seated[core].load();
    g_affinity_seated[core] = static_cast<uint8_t>(seated);
    return was;
}

uint32_t kickos_armv8a_gic_deferred(uint32_t core)
{
    if (core >= KICKOS_DOORBELL_CORES)
    {
        return 0;
    }
    return g_deferred[core].load();
}
#endif

void kickos_armv8a_gic_doorbell_send(uint32_t cores)
{
    // The ring is the authority and this raise is a hint: a core whose affinity is unpublished
    // cannot be targeted, so its publication stands and only its notice is deferred, that peer
    // draining what it was sent before it waits on a doorbell of any kind.
    //
    // The seating flag is MONOTONIC, only ever going unseated to seated, so a load reading
    // seated is never stale and needs no barrier. A load reading unseated may be, and skipping
    // on a stale one strands a publication with no notice and no later scan, so that decision
    // alone is made behind a full barrier, pairing with the one the peer runs between seating
    // and draining.
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
            continue;
        }
#if defined(KICKOS_ENABLE_SELFTEST)
        g_deferred[index].store(g_deferred[index].load() + 1u);
#endif
    }
    if (pending == 0)
    {
        return;
    }
    // THE ARCHITECTURE ORDERS AN SGI AGAINST NOTHING, so the far side's view of earlier writes
    // is the memory model's to order.
    __asm volatile("dsb ish" ::: "memory");

    while (pending != 0)
    {
        uint32_t lead = 0;
        while ((pending & (1u << lead)) == 0)
        {
            lead++;
        }
        // The lead leaves `pending` HERE and not in the sweep below, whose every other exit is
        // a `continue`: a bound or a predicate that stopped agreeing with the mask would else
        // leave the lead bit set and spin this core masked, which is a hang, not a lost raise.
        pending &= ~(1u << lead);
        uint32_t const packed = g_affinity[lead].load();
        uint32_t const cluster = packed & 0xFFFFFF00u;
        uint32_t const rs = (packed & 0xFFu) / SGI_TARGETS_PER_WINDOW;
        uint32_t targets = 1u << ((packed & 0xFFu) % SGI_TARGETS_PER_WINDOW);
        // The partition's width and not this image's (docs/design-multicore.md N6c): the mask
        // names machine cores, and an own-image AMP node drives ONE while naming peers on
        // others, so a bound from KICKOS_NUM_CORES is 1 on the one posture with somewhere to
        // raise.
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
    // The raise is a system register write, so it is not in effect until the context
    // synchronises.
    __asm volatile("isb" ::: "memory");
}

// A GICv3 SGI has one pending bit per target rather than one per source pair, so the whole
// doorbell drops in a single store on this core's own redistributor.
void kickos_armv8a_gic_doorbell_clear(void)
{
    *gicr32(my_rd_base(), GICR_ICPENDR0) = 1u << (GIC_SGI_DOORBELL % 32);
}

// This core's banked enable word, doorbell bit excepted: that word covers the SGI and PPI IDs,
// which are the only ones the redistributor holds.
void kickos_armv8a_gic_doorbell_only(void)
{
    uintptr_t const base = my_rd_base();
    *gicr32(base, GICR_ICENABLER0) = ~(1u << (GIC_SGI_DOORBELL % 32));
    // The caller parks with interrupts OPEN straight after this, so a still-pending disable
    // would let the timer PPI into a core that reaches no scheduler.
    wait_gicr_rwp(base);
}
#endif

// Write-1-to-ACT, so the pending state of one INTID drops with a single aligned store.
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
// Self-bracketed per arch.h: the enable and pending registers are write-1-to-ACT, so no body
// here read-modify-writes and every store is single and aligned. Unmask writes the ENABLE
// last, so a half-applied sequence leaves the line masked.
//
// MASK COSTS A COMPLETION WAIT AND THE REST OF THE TRIAD DOES NOT: RWP tracks a cleared enable
// and nothing else, so unmask, inject and clear_pending have no bit to poll. Without that wait
// this seam returns with delivery still possible, and the first-level ISR masks a line
// precisely to stop it.
//
// WHICH CORE THIS FAMILY ACTS ON IS THE INTID'S. A line below GIC_BANKED_INTIDS is held in the
// calling core's own redistributor, so the same argument means a different interrupt on each
// core; a line at or above it is global and arch_irq_unmask routes it to core 0.
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
        // BYTE per INTID: a word index programs a different interrupt, and a 32-bit access is
        // also unaligned, which on Device memory faults.
        *gicr8(my_rd_base(), GICR_IPRIORITYR + static_cast<uintptr_t>(line)) = 0;
        *gicr32(my_rd_base(), GICR_ISENABLER0) = 1u << (line % 32);
        return;
    }
    *gicd8(GICD_IPRIORITYR + static_cast<uintptr_t>(line)) = 0;
    // A global interrupt reaches no core until one is named, and core zero's own PUBLISHED
    // affinity is the only thing that names it.
    //
    // DELIBERATELY UNLIKE THE GICv2 BACKEND, WHICH ENABLES THE LINE WITH AN EMPTY TARGET BYTE.
    // A GICv2 target list is one-hot, so zero names nobody and an enabled line with no target
    // is expressible; affinity zero is a real core rather than an absence, so it is not. The
    // line is left MASKED instead, which refuses rather than routing to a guess. Unreachable
    // in practice, the primary publishing inside arch_init before anything here can run.
    if (g_affinity_seated[arch_doorbell_core()].load() == 0)
    {
        return;
    }
    *gicd64(GICD_IROUTER + static_cast<uintptr_t>(line) * 8) =
        router_value(g_affinity[arch_doorbell_core()].load());
    *gicd32(GICD_ISENABLER + (line / 32) * 4) = 1u << (line % 32);
}

void arch_irq_clear_pending(int line)
{
    if (line < 0 or line >= kickos_gicv3.intid_count)
    {
        return;
    }
    kickos_armv8a_gic_clear_pending(line);
}

// Test scaffolding (arch.h). The set-pending registers pend in the controller, so delivery
// takes the ordinary path and the latch-while-masked contract needs no software shadow.
void arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= kickos_gicv3.intid_count)
    {
        return;
    }
    if (irq < GIC_BANKED_INTIDS)
    {
        *gicr32(my_rd_base(), GICR_ISPENDR0) = 1u << (irq % 32);
        return;
    }
    *gicd32(GICD_ISPENDR + (irq / 32) * 4) = 1u << (irq % 32);
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
        kickos_arm64_doorbell_service();
    }
#endif
    else
    {
        kickos_isr_irq(static_cast<int>(intid));
    }
    // THE INTID ALONE: a GICv3 acknowledge carries no source field for an SGI, so what was
    // read back is what is ended.
    __asm volatile("msr icc_eoir1_el1, %0" ::"r"(static_cast<uint64_t>(intid)));
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
