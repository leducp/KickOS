// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/reent.h>

#if KICKOS_LIBC_REENT

#include <kickos/config/system.h> // KICKOS_THREAD_SLOTS
#include <kickos/instance_local.h>
#include <kickos/kruntime.h>
#include <kickos/aspace.h>
#include <kickos/kernel.h>

#if defined(KICKOS_ENABLE_SELFTEST) or KICKOS_HAVE_ASPACE
#include <kickos/sched.h>
#endif

namespace kickos
{
    namespace
    {
        KickosReentSeam s_seam = {};

        // At namespace scope and volatile: a local volatile would stop the value folding but not
        // the address being materialised inline, which two gates refuse
        // (tests/static/check_riscv_kernel_apphalf.sh, check_riscv_kernel_gp.sh).
        KickosReentSeam const* const volatile s_seam_home = &kickos_reent_seam;

#if defined(KICKOS_ENABLE_SELFTEST)
        // Writes to the app half made for a thread whose memory view is not installed.
        size_t s_unseated_writes = 0;

        void note_write(void)
        {
            if (not aspace_seated_for(sched::current()))
            {
                s_unseated_writes++;
            }
        }
#endif
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    size_t reent_unseated_writes(void)
    {
        return s_unseated_writes;
    }
#endif

    void reent_seam_read(void)
    {
        // Through the kernel's own alias where the image is split: the descriptor is app-side
        // storage read before any address space exists.
        KickosReentSeam const* src = s_seam_home;
#if KICKOS_HAVE_ASPACE
        KickosReentSeam const* const alias =
            static_cast<KickosReentSeam const*>(aspace_image_alias(s_seam_home));
        if (alias != nullptr)
        {
            src = alias;
        }
#endif
        s_seam = *src;

        // REFUSED AT BOOT, because the fallback below is an ALIAS and not an error: a
        // descriptor short of one bank per instance resolves two kernels' slots to one
        // struct _reent, so they share an errno and nothing on any later path says so.
        size_t const banked =
            static_cast<size_t>(KICKOS_MAX_INSTANCES) * KICKOS_THREAD_SLOTS;
        if (s_seam.count < 0 or static_cast<size_t>(s_seam.count) < banked)
        {
            kpanic("libc reentrant-state descriptor is short of "
                   "KICKOS_MAX_INSTANCES * KICKOS_THREAD_SLOTS slots");
        }
    }

    void* reent_state_for_slot(int slot)
    {
        if (slot < 0)
        {
            return s_seam.shared;
        }
        // ONE BANK PER INSTANCE (reent.h). The bound is still read: reent_seam_read refused a
        // descriptor too short for the banking, so what reaches here is a slot out of range.
        size_t const index =
            static_cast<size_t>(kickos_instance_index()) * KICKOS_THREAD_SLOTS
            + static_cast<size_t>(slot);
        if (index >= static_cast<size_t>(s_seam.count))
        {
            return s_seam.shared;
        }
        return static_cast<unsigned char*>(s_seam.slots) + index * s_seam.stride;
    }

    void reent_prime(struct arch_aspace* space, void* state)
    {
        // s_seam.shared MUST stay pristine: reent_state_for_slot hands it out only for a TCB
        // outside the pool, and seating it on a thread that prints would make every later
        // prime inherit that thread's leftovers.
#if defined(KICKOS_ENABLE_SELFTEST)
        note_write();
#endif
#if KICKOS_HAVE_ASPACE
        // Both ends are app-half in one space, which is ep_copy's shape. They must be
        // disjoint, as one space requires: the slot array and the process-wide state are
        // different objects.
        if (not ep_copy(space, reinterpret_cast<uintptr_t>(state), space,
                        reinterpret_cast<uintptr_t>(s_seam.shared), s_seam.stride))
        {
            thread_cancel_escalate(sched::current(), CANCEL_SLAY);
        }
#else
        (void)space;
        kmemcpy(state, s_seam.shared, s_seam.stride);
#endif
    }

    void reent_seat(struct arch_aspace* space, void* state)
    {
#if defined(KICKOS_ENABLE_SELFTEST)
        note_write();
#endif
#if KICKOS_HAVE_ASPACE
        // A silent refusal here leaves the seat word naming the outgoing thread's block, so
        // two processes resolve one errno and one stdio state through it.
        if (not kaccess_to_user(space, reinterpret_cast<uintptr_t>(s_seam.seat), &state,
                                sizeof(state)))
        {
            thread_cancel_escalate(sched::current(), CANCEL_SLAY);
        }
#else
        // -ffreestanding, so the ordinary memcpy name is never expanded and a pointer-width
        // copy would lower to a call on every switch. The descriptor carries the word as
        // void*, hence the alignment hint.
        (void)space;
        void** const word =
            static_cast<void**>(__builtin_assume_aligned(s_seam.seat, sizeof(void*)));
        __builtin_memcpy(word, &state, sizeof(state));
#endif
    }
}

#endif
