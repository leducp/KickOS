// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The deferred-commit stash of every backend whose switch is pended (PendSV on ARM, msip on
// rv32imac), in a member of its own so a host gate can drive it (tests/unit/mpuskip).
//
// arch_mpu_apply runs on the OUTGOING thread and the physical swap comes later: programming the
// hardware there would run the outgoing thread under the INCOMING thread's regions. It only
// stashes the set; kickos_arch_mpu_commit programs it after the swap.

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU

// A POINTER into the incoming thread's TCB, not a copy: a re-encode between the stash and the
// commit must reach the commit. Thread slots are a static pool, so a pointer left from an
// earlier switch still addresses valid storage; of two switch_book calls under one lock the
// second names the thread the switch lands on.
//
// Global for rv32imac's commit, which reads it in place: a call would cost the msip switch
// path a frame its red zone does not reserve.
extern "C"
{
    struct arch_mpu_encoded const* g_pend_image = nullptr;
}

extern "C" struct arch_mpu_encoded const* kickos_arm_mpu_pending(void)
{
    return g_pend_image;
}

// A chip replaces only the commit, never this, so this definition is not overridable.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    g_pend_image = image;
}

// THE STASH IS RESTORED: a switch may already be pended behind the caller and the stash is one
// cell, so leaving this set in it would program the CALLER's regions onto the incoming thread.
// Self-bracketed: an interrupt between the two writes could stash for a switch, and the restore
// would drop that image.
void arch_mpu_apply_now(struct arch_mpu_region const* regions, size_t n,
                        struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    arch_irq_state_t const irq = arch_irq_save();
    struct arch_mpu_encoded const* const pend = g_pend_image;
    g_pend_image = image;
    kickos_arch_mpu_commit();
    g_pend_image = pend;
    arch_irq_restore(irq);
}

#else

// The two apply symbols must still resolve.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    (void)image;
}

void arch_mpu_apply_now(struct arch_mpu_region const* regions, size_t n,
                        struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    (void)image;
}

#endif
