// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/reent.h>

#if !KICKOS_ARCH_SIM

#include <kickos/kruntime.h>
#include <kickos/aspace.h>

#if defined(KICKOS_ENABLE_SELFTEST)
#include <kickos/sched.h>
#endif

namespace kickos
{
    namespace
    {
        // THE KERNEL'S OWN COPY, and the reason the descriptor is a seam rather than a
        // direct read. Everything below answers out of this, so the linker split has only
        // to change what fills it.
        KickosReentSeam s_seam = {};

        // A LINK-TIME WORD, at namespace scope: the descriptor is app-half storage, and kernel
        // text materialising an app-half address on a split image links SILENTLY, so two gates
        // and not the link refuse it (tests/static/check_riscv_kernel_apphalf.sh and
        // check_riscv_kernel_gp.sh). A local volatile stops the VALUE being folded and not the
        // address being materialised inline.
        KickosReentSeam const* const volatile s_seam_home = &kickos_reent_seam;

#if defined(KICKOS_ENABLE_SELFTEST)
        // Writes to the app half made for a thread whose memory view is not installed. The
        // check sits HERE and the guard sits in the switch path, so a guard that stops
        // guarding is counted rather than silently correct.
        //
        // WHAT IT WITNESSES IS WEAKER SINCE THE WRITES WENT THROUGH THE kaccess SEAM: the seam
        // reaches the space it is HANDED, so such a write no longer lands in whichever process
        // was installed last. It stays because the switch path's guard also covers the thread
        // that has no space at all, where there is nothing to write into.
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
        // THROUGH A LINK-TIME WORD, AND THROUGH THE KERNEL'S OWN ALIAS OF THOSE BYTES where
        // the image is split: the descriptor is app-side storage read before any address
        // space exists, so its own virtual address names nothing yet on a backend whose app
        // window is not linked where it loads.
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
    }

    void* reent_state_for_slot(int slot)
    {
        if (slot < 0 or slot >= s_seam.count)
        {
            return s_seam.shared;
        }
        return static_cast<unsigned char*>(s_seam.slots)
               + static_cast<size_t>(slot) * s_seam.stride;
    }

    void reent_prime(struct arch_aspace* space, void* state)
    {
        // THE PRISTINE IMAGE IS THE PROCESS-WIDE STATE ITSELF, which is why the seam needs
        // no template of its own. libc statically initialises it (_impure_data, a .data
        // object whose only relocations point at the shared __sf), so it holds exactly the
        // post-boot contents a slot must be brought to, with no pointer into itself that a
        // byte copy would carry to the wrong owner.
        //
        // WHAT KEEPS IT PRISTINE is that no thread which uses libc is ever seated on it:
        // reent_state_for_slot hands it out only for a TCB outside the pool, which is idle,
        // and idle holds no capability and runs arch_idle_wait alone. Seating it on a thread
        // that prints would make every later prime inherit that thread's leftovers.
#if defined(KICKOS_ENABLE_SELFTEST)
        note_write();
#endif
#if KICKOS_HAVE_ASPACE
        // BOTH ENDS ARE APP-HALF IN ONE SPACE, which is ep_copy's shape and not
        // kaccess_to_user's. Disjoint, as one space requires: the slot array and the
        // process-wide state are different objects.
        ep_copy(space, reinterpret_cast<uintptr_t>(state), space,
                reinterpret_cast<uintptr_t>(s_seam.shared), s_seam.stride);
#else
        (void)space;
        kmemcpy(state, s_seam.shared, s_seam.stride);
#endif
    }

    void reent_seat(struct arch_aspace* space, void* state)
    {
        // IT IS A COPY AND NOT A STORE, for the two reasons reent.h gives.
#if defined(KICKOS_ENABLE_SELFTEST)
        note_write();
#endif
#if KICKOS_HAVE_ASPACE
        kaccess_to_user(space, reinterpret_cast<uintptr_t>(s_seam.seat), &state,
                        sizeof(state));
#else
        // __builtin_memcpy AND NOT kmemcpy, THE ONE PLACE IN THE KERNEL THAT SPELLS IT SO.
        // The build is -ffreestanding, so the ordinary name is never expanded and a
        // pointer-width copy would lower to a CALL on every switch; the builtin spelling is
        // expanded regardless and leaves this a leaf. The alignment hint is the other half:
        // the descriptor carries the word as void*, which discards what the app knew, and an
        // unaligned pointer-width copy is a call again. The seam route above is a call by
        // construction and this reason does not reach it.
        (void)space;
        void** const word =
            static_cast<void**>(__builtin_assume_aligned(s_seam.seat, sizeof(void*)));
        __builtin_memcpy(word, &state, sizeof(state));
#endif
    }
}

#endif
