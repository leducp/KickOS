// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/reent.h>

#if !KICKOS_ARCH_SIM

#include <kickos/kruntime.h>

namespace kickos
{
    namespace
    {
        // THE KERNEL'S OWN COPY, and the reason the descriptor is a seam rather than a
        // direct read. Everything below answers out of this, so the linker split has only
        // to change what fills it.
        KickosReentSeam s_seam = {};
    }

    void reent_seam_read(void)
    {
        s_seam = kickos_reent_seam;
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

    void reent_prime(void* state)
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
        kmemcpy(state, s_seam.shared, s_seam.stride);
    }

    void reent_seat(void* state)
    {
        // __builtin_memcpy AND NOT kmemcpy, THE ONE PLACE IN THE KERNEL THAT SPELLS IT SO.
        // The build is -ffreestanding, so the ordinary name is never expanded and a
        // pointer-width copy would lower to a CALL on every switch; the builtin spelling is
        // expanded regardless and leaves this a leaf. The alignment hint is the other half:
        // the descriptor carries the word as void*, which discards what the app knew, and an
        // unaligned pointer-width copy is a call again.
        //
        // IT IS STILL A COPY AND NOT A STORE, for the two reasons reent.h gives.
        void** const word =
            static_cast<void**>(__builtin_assume_aligned(s_seam.seat, sizeof(void*)));
        __builtin_memcpy(word, &state, sizeof(state));
    }
}

#endif
