// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The deferred-commit stash of every backend whose switch is pended (PendSV on ARM, msip on
// rv32imac), in an archive member of its own so a host gate can drive it (tests/unit/mpuskip).
//
// arch_mpu_apply runs on the OUTGOING thread, but the physical swap happens only later in the
// pended switch. Programming the hardware eagerly would run the outgoing thread under the
// INCOMING thread's regions until then, a fault on its own stack (proven on RP2040). So
// arch_mpu_apply only STASHES the region set here; kickos_arch_mpu_commit programs the
// hardware AFTER the swap.

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU

// A POINTER into the incoming thread's TCB, not a copy. The image is re-encoded by its owner
// alone, and that owner is the thread the pended switch will land on, so a re-encode between
// the stash and the commit programs the set that thread actually has. Thread slots come from
// a static pool and are never returned to an allocator, so a pointer left over from an
// earlier switch still addresses valid storage; two switch_book calls under one lock leave
// the second, which is the thread the switch lands on.
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

// THE STASH IS RESTORED: a switch to another thread may already be pended behind the caller,
// and the stash is ONE cell. Leaving this set in it would have the commit program the CALLER's
// regions onto the incoming thread, which would then run under them until the next switch
// re-stashed. Self-bracketed: an interrupt between the two writes below could decide a switch,
// and the restore would then drop the image that switch stashed.
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

// The stash has no reader, but the two apply symbols must still resolve.
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
