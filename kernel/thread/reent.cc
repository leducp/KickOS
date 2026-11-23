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
#include <kickos/task.h>
#include <kickos/thread.h>

#if KICKOS_HAVE_ASPACE and not KICKOS_REENT_PER_THREAD
#error "the switch seats one word directly, which only a region backend's kernel can write"
#endif
#if not KICKOS_REENT_PER_THREAD and not KICKOS_ARCH_SWITCH_LANDS_LATER
#error "the word is seated where the switch lands, after the incoming regions are committed"
#endif

namespace kickos
{
    namespace
    {
        // The cell: the carve's last KICKOS_REENT_CELL_WORDS words, ending where the TLS block
        // starts. The user runtime's own words, then the state's address, then the lock id.
        constexpr size_t CELL = KICKOS_REENT_CELL_WORDS * sizeof(void*);
        constexpr size_t RUNTIME_WORDS = KICKOS_REENT_CELL_WORDS - 2;
    }

    void reent_seam_read(size_t, kos_image_header const& h)
    {
        size_t const carve = static_cast<size_t>(h.reent_carve);
        if (carve < CELL or carve % KICKOS_STACK_ALIGN != 0)
        {
            kpanic("an image's libc state carve is off the stack alignment or holds no cell");
        }
        // Every stack sum the kernel forms with the carve rests on this bound.
        if (carve > KICKOS_REENT_CARVE_MAX)
        {
            kpanic("an image's libc state carve is larger than every profile's");
        }
        // A prime copies `width` bytes from the carve's base, so a wider state runs into the
        // cell and the lock id the kernel wrote there.
        if (static_cast<size_t>(h.reent_width) > carve - CELL)
        {
            kpanic("libc reentrant state is wider than its image's carve leaves below its cell");
        }
    }

    void* reent_shared_state(void)
    {
        return reinterpret_cast<void*>(image_record(0).reent_template);
    }

    void reent_cell_seat(void* home, void* state, uint32_t self, size_t carve)
    {
        unsigned char* const cell = static_cast<unsigned char*>(
            __builtin_assume_aligned(home, sizeof(void*))) + (carve - CELL);
        uintptr_t const id = static_cast<uintptr_t>(self);
        // A reused stack block still holds its last thread's words.
        kmemset(cell, 0, RUNTIME_WORDS * sizeof(void*));
        unsigned char* const seat = cell + RUNTIME_WORDS * sizeof(void*);
        __builtin_memcpy(seat, &state, sizeof(state));
        __builtin_memcpy(seat + sizeof(void*), &id, sizeof(id));
    }

    void reent_prime(void* home, size_t image)
    {
        kos_image_header const& h = image_record(image);
        // The image check places a template only for a nonzero width.
        if (h.reent_width == 0)
        {
            return;
        }
        // From the image's copy of its .data, which no task writes: the live state may be a past
        // incarnation's, or not yet copied in.
#if KICKOS_HAVE_ASPACE
        void const* const pristine
            = aspace_image_alias(image, reinterpret_cast<void const*>(h.reent_template));
        KICKOS_ASSERT(pristine != nullptr);
#else
        void const* const pristine
            = reinterpret_cast<void const*>(h.data_load + (h.reent_template - h.data_start));
#endif
        kmemcpy(home, pristine, static_cast<size_t>(h.reent_width));
    }

#if not KICKOS_REENT_PER_THREAD
    void reent_seat(Thread const* t)
    {
        Task const* const tk = t->task;
        if (tk == nullptr or tk->reent_seat == nullptr)
        {
            return;
        }
        // __builtin_memcpy: under -ffreestanding a plain memcpy lowers to a call on every switch.
        void** const word
            = static_cast<void**>(__builtin_assume_aligned(tk->reent_seat, sizeof(void*)));
        __builtin_memcpy(word, &t->reent, sizeof(t->reent));
    }
#endif
}

#endif
