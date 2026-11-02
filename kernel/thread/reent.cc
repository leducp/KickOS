// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/reent.h>

#if KICKOS_LIBC_REENT

#include <kickos/aspace.h>
#include <kickos/config/system.h> // KICKOS_STACK_ALIGN
#include <kickos/image.h>
#include <kickos/kernel.h>
#include <kickos/kruntime.h>
#include <kickos/sys/image.h>

#if KICKOS_HAVE_ASPACE
#include <kickos/domain.h>
#include <kickos/sched.h>
#endif

#if KICKOS_HAVE_ASPACE and not KICKOS_REENT_PER_THREAD
#error "the switch seats one word directly, which only a region backend's kernel can write"
#endif

namespace kickos
{
    namespace
    {
        // The cell: the carve's last two words, the state's address then the lock id, which
        // end where the TLS block starts.
        constexpr size_t CELL = 2 * sizeof(void*);
        static_assert(KICKOS_REENT_CARVE >= CELL, "the libc state carve ends in the cell");
        static_assert(KICKOS_REENT_CARVE % KICKOS_STACK_ALIGN == 0,
                      "the TLS block above the libc state starts on the stack alignment");
    }

    void reent_seam_read(size_t, kos_image_header const& h)
    {
        // A prime copies `width` bytes from the carve's base, so a wider state runs into the
        // cell and the lock id the kernel wrote there.
        if (static_cast<size_t>(h.reent_width) > KICKOS_REENT_CARVE - CELL)
        {
            kpanic("libc reentrant state is wider than KICKOS_REENT_CARVE leaves below its cell");
        }
    }

    void* reent_shared_state(void)
    {
        return reinterpret_cast<void*>(image_record(0).reent_template);
    }

    void reent_cell_seat(void* home, void* state, uint32_t self)
    {
        unsigned char* const cell = static_cast<unsigned char*>(
            __builtin_assume_aligned(home, sizeof(void*))) + (KICKOS_REENT_CARVE - CELL);
        uintptr_t const id = static_cast<uintptr_t>(self);
        __builtin_memcpy(cell, &state, sizeof(state));
        __builtin_memcpy(cell + sizeof(void*), &id, sizeof(id));
    }

    void reent_prime(struct arch_aspace* space, size_t image, void* state)
    {
        kos_image_header const& h = image_record(image);
        // From the image's copy of its .data, which no task writes: the live state may be a past
        // incarnation's, or not yet copied in.
#if KICKOS_HAVE_ASPACE
        void const* const pristine
            = aspace_image_alias(image, reinterpret_cast<void const*>(h.reent_template));
        if (pristine == nullptr
            or not kaccess_to_user(space, reinterpret_cast<uintptr_t>(state), pristine,
                                   static_cast<size_t>(h.reent_width)))
        {
            thread_cancel_escalate(sched::current(), CANCEL_SLAY);
        }
#else
        (void)space;
        uintptr_t const pristine = h.data_load + (h.reent_template - h.data_start);
        kmemcpy(state, reinterpret_cast<void const*>(pristine),
                static_cast<size_t>(h.reent_width));
#endif
    }

#if not KICKOS_REENT_PER_THREAD
    void reent_seat(void* seat, void* state)
    {
        // __builtin_memcpy: under -ffreestanding a plain memcpy lowers to a call on every switch.
        void** const word = static_cast<void**>(__builtin_assume_aligned(seat, sizeof(void*)));
        __builtin_memcpy(word, &state, sizeof(state));
    }
#endif
}

#endif
