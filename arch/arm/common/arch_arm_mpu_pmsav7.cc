// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ARM PMSAv7 descriptor writer, shared by ARMv6-M and ARMv7-M (one register map).
// Its own archive member so a host gate can drive it over a register window of its own
// (tests/unit/mpuskip) and so a SYSMPU or PMSAv8 board, which programs its descriptors
// from its own commit, links none of it.
//
// A commit writes only the descriptors whose words changed. The record of what the
// hardware holds is what makes that sound, and keeping it in step is this file's whole
// burden: see kickos_arm_mpu_program below.

#include <kickos/arch/arch.h>

#include "regs.h"
#include "mpu.h"

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU

namespace
{
    using namespace kickos::arm;
}

// Descriptor slots a per-thread image can occupy, above the chip's fixed rows. Both entry
// points bound-check the silicon against it and refuse a part that cannot hold them all.
static constexpr size_t MAX_PEND_REGIONS = ARCH_MPU_ENCODED_SLOTS;

// Count of chip fixed regions occupying the LOW MPU slots [0, g_fixed_count).
// Set once by kickos_arm_mpu_fixed_init; per-thread grants are programmed ABOVE it.
// 0 for every chip without a fixed-region hook -> those chips are byte-identical.
static size_t g_fixed_count = 0;

#if KICKOS_ARM_MPU == KICKOS_ARM_MPU_PMSAV7

// The descriptor words this core's MPU holds, and whether that record may be believed.
// A commit programs only the slots whose words differ from it, so ANYTHING that writes or
// suspends a descriptor outside kickos_arm_mpu_program must invalidate the record first:
// a partial commit leaves a disagreement standing where a total one erased it.
//
// NOT the pending image POINTER. MpuSet's mutators re-encode in place and thread slots come
// from a static pool, so the same pointer can address different words later; only a copy of
// the WORDS answers what the hardware holds.
static struct arch_mpu_encoded g_mpu_held[KICKOS_NUM_CORES];
static bool g_mpu_held_valid[KICKOS_NUM_CORES];

static void mpu_held_invalidate(void)
{
    g_mpu_held_valid[arch_cpu_id()] = false;
}

// One descriptor, written in the order the hardware needs: the slot is disabled before its
// base moves, or it would briefly pair the new base with the old size and attributes.
//
// noinline is LOAD-BEARING: the diff below is UNROLLED, so an inlined body would be emitted
// eight times over.
static __attribute__((noinline)) void mpu_write_slot(uint32_t slot, uint32_t rbar, uint32_t rasr)
{
    reg32(MPU_RNR) = slot;
    reg32(MPU_RASR) = 0;
    reg32(MPU_RBAR) = rbar;
    reg32(MPU_RASR) = rasr;
}

// The MPU hardware-programming step (reprogram descriptors / enable). Split out of
// arch_mpu_apply so the PendSV epilogue can run it atomically with the physical context
// switch. An eager apply reprograms the MPU for the incoming thread while the outgoing
// thread is still running (PendSV not fired yet), faulting it on its own stack.
//
// A TOTAL commit (the record is not to be believed) writes every descriptor and enables the
// MPU; a PARTIAL one writes only the slots whose words moved and leaves the rest standing,
// which also narrows the reprogram window to those slots.
//
// THE DIFF IS UNROLLED AND THE TWO POSTURES ARE SEPARATE BODIES, and both are worth their
// text. Written as one loop carrying the posture test, -Os re-reads the record flag and
// re-checks the slot bound every iteration, so a SKIPPED descriptor costs 14 instructions and
// three taken branches, which is most of what the skip was buying back; unrolled it costs
// eight and one. Counted on f411disco-bench at M8.11, where the whole commit was 247 cycles
// writing all eight.
static_assert(ARCH_MPU_ENCODED_SLOTS == 8, "the diff below is unrolled slot by slot");

extern "C" void kickos_arm_mpu_program(struct arch_mpu_encoded const* img)
{
    if (img == nullptr)
    {
        return;
    }
    uint32_t const core = arch_cpu_id();
    struct arch_mpu_encoded* const held = &g_mpu_held[core];
    // Chip fixed regions own the LOW slots [0, k), programmed once by
    // kickos_arm_mpu_fixed_init and NEVER touched here. Per-thread grants go in
    // [k, hw), so a grant sits ABOVE the fixed background and correctly overrides it
    // (PMSAv7: highest-numbered region wins). k == 0 on every chip without a fixed hook.
    uint32_t const k = static_cast<uint32_t>(g_fixed_count);

#define KICKOS_ARM_MPU_SLOT(j)                                                               \
    if (held->rbar[j] != img->rbar[j] or held->rasr[j] != img->rasr[j])                      \
    {                                                                                        \
        mpu_write_slot(k + (j), img->rbar[j], img->rasr[j]);                                 \
        held->rbar[j] = img->rbar[j];                                                        \
        held->rasr[j] = img->rasr[j];                                                        \
        wrote = true;                                                                        \
    }

    if (g_mpu_held_valid[core])
    {
        bool wrote = false;
        KICKOS_ARM_MPU_SLOT(0)
        KICKOS_ARM_MPU_SLOT(1)
        KICKOS_ARM_MPU_SLOT(2)
        KICKOS_ARM_MPU_SLOT(3)
        KICKOS_ARM_MPU_SLOT(4)
        KICKOS_ARM_MPU_SLOT(5)
        KICKOS_ARM_MPU_SLOT(6)
        KICKOS_ARM_MPU_SLOT(7)
        if (not wrote)
        {
            return; // no descriptor moved, so there is nothing to synchronise
        }
        barrier_dsb();
        barrier_isb();
        return;
    }
#undef KICKOS_ARM_MPU_SLOT

    // MEMFAULTENA and BUSFAULTENA keep an isolation violation and a bus abort as MemManage and
    // BusFault instead of letting either escalate to HardFault.
    //
    // MPU_CTRL IS DELIBERATELY NOT ZEROED HERE. Disabling the MPU also stops the CHIP FIXED
    // rows applying, and on imxrt1062 those carry the ERR011573 anti-speculation wrap over
    // the FlexSPI band this code is itself executing from. Each descriptor is instead
    // disabled individually just before it is rewritten, which leaves [0, k) in force
    // throughout.
    reg32(SCB_SHCSR) |= SHCSR_MEMFAULTENA | SHCSR_BUSFAULTENA;
    barrier_dmb();
    size_t const hw_regions = (reg32(MPU_TYPE) >> 8) & 0xFFu;
    // The per-thread slots the unrolled diff above writes MUST exist in the silicon, or a
    // grant would silently fall off the top. Same refusal and same reason as
    // kickos_arm_mpu_fixed_init's, which only ever asked it of a chip declaring fixed rows.
    // PMSAv7 and PMSAv6-M implement 8 or 16 descriptors where an MPU is present at all, so
    // this holds on every part in the fleet. No kernel assert on the arch path -> spin.
    if (k + MAX_PEND_REGIONS > hw_regions)
    {
        while (true)
        {
            wait_for_interrupt();
        }
    }
    for (size_t j = 0; j < ARCH_MPU_ENCODED_SLOTS; j++)
    {
        mpu_write_slot(static_cast<uint32_t>(k + j), img->rbar[j], img->rasr[j]);
        held->rbar[j] = img->rbar[j];
        held->rasr[j] = img->rasr[j];
    }
    // Past the image, so no commit ever gives these a value: disabled once here and never
    // written again.
    for (size_t i = k + MAX_PEND_REGIONS; i < hw_regions; i++)
    {
        reg32(MPU_RNR) = static_cast<uint32_t>(i);
        reg32(MPU_RASR) = 0;
    }
    g_mpu_held_valid[core] = true;
    barrier_dsb();
    // Do not drop this because kickos_arm_mpu_fixed_init also enables the MPU: only imxrt1062
    // calls that, so on every other PMSAv7 chip this is the ONLY write that enables it. It
    // enables, so unlike a leading MPU_CTRL = 0 it cannot stop the chip fixed rows applying.
    reg32(MPU_CTRL) = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
    barrier_dsb();
    barrier_isb();
}

#else

// The record belongs to the program above. A SYSMPU or PMSAv8 board keeps its own beside
// its own commit, and fixed_init below calls this on every revision.
static void mpu_held_invalidate(void)
{
}

#endif // KICKOS_ARM_MPU_PMSAV7

// One-time: program the chip's fixed regions into the LOW slots [0, k), cache k, and
// enable the MPU (with the PRIVDEFENA background). Call from the chip arch_init BEFORE
// enabling caches and before the scheduler starts. Idempotent-safe to call once.
extern "C" void kickos_arm_mpu_fixed_init(void)
{
    struct kickos_arm_mpu_fixed_region const* fixed = nullptr;
    size_t const k = kickos_arm_mpu_fixed(&fixed);
    size_t const hw_regions = (reg32(MPU_TYPE) >> 8) & 0xFFu;
    // The fixed set plus a full per-thread set must fit the hardware descriptors, or a
    // per-thread grant would silently fall off the top. Fail loud (a chip-config bug
    // caught at boot), never truncate. No kernel assert on the arch path -> spin.
    if (k + MAX_PEND_REGIONS > hw_regions)
    {
        while (true)
        {
            wait_for_interrupt();
        }
    }
    // This suspends every descriptor and moves the low ones, and it moves the boundary the
    // per-thread slot index is taken from, so nothing the record claimed still holds.
    mpu_held_invalidate();
    // Zeroing MPU_CTRL is correct HERE and only here: this runs at boot, before the caches and
    // before any thread, so there is no fixed row yet to lose.
    reg32(SCB_SHCSR) |= SHCSR_MEMFAULTENA | SHCSR_BUSFAULTENA;
    reg32(MPU_CTRL) = 0;
    barrier_dsb();
    for (size_t i = 0; i < k; i++)
    {
        reg32(MPU_RNR) = static_cast<uint32_t>(i);
        reg32(MPU_RBAR) = fixed[i].base & ~0x1Fu;
        reg32(MPU_RASR) = fixed[i].rasr;
    }
    g_fixed_count = k;
    barrier_dsb();
    reg32(MPU_CTRL) = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
    barrier_dsb();
    barrier_isb();
}

#endif // KICKOS_HAVE_MPU
