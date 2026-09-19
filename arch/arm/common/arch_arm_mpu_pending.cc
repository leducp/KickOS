// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The deferred-commit stash shared by every ARM backend, in an archive member of its own so
// a host gate can drive it (tests/unit/mpuskip) beside the descriptor writer it feeds.
//
// The switch is a PENDED PendSV on every ARM arch: arch_mpu_apply runs on the OUTGOING
// thread, but the physical register/PSP swap only happens later in PendSV. Programming the
// hardware eagerly would run the outgoing thread under the INCOMING thread's regions until
// PendSV fires -> a fault on its own stack (proven on RP2040). So arch_mpu_apply only
// STASHES the region set here; kickos_arch_mpu_commit programs the hardware AFTER the swap.

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
static struct arch_mpu_encoded const* g_pend_image = nullptr;

// Read the pending stash. Lets a chip whose MPU is NOT PMSAv7 (K64F SYSMPU, PMSAv8) program
// its own hardware from the SAME stash by defining only the commit.
extern "C" struct arch_mpu_encoded const* kickos_arm_mpu_pending(void)
{
    return g_pend_image;
}

// STASH-ONLY apply: record the incoming image, no hardware write. Shared by every ARM
// backend, PMSAv7 (v6-M/v7-M) and K64F SYSMPU and PMSAv8 alike; a chip replaces only the
// commit, never this, so this definition is not overridable.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    g_pend_image = image;
}

// THE STASH IS RESTORED, and that is this entry's whole reason to exist: a switch to another
// thread may already be pended behind the caller, and the stash is ONE cell. Leaving this set
// in it would have the epilogue program the CALLER's regions onto the incoming thread, which
// would then run under them until the next switch re-stashed. Self-bracketed: an interrupt
// between the two writes below could decide a switch, and the restore would then drop the
// image that switch stashed.
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

// No enforcement on this board (KICKOS_HAVE_MPU=0): privilege + SVC only. The stash has no
// reader, but the two apply symbols must still resolve.
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
