// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/reent.h>

#if KICKOS_LIBC_REENT

#include <kickos/config/system.h> // KICKOS_THREAD_SLOTS
#include <kickos/instance_local.h>
#include <kickos/kruntime.h>
#include <kickos/aspace.h>
#include <kickos/kernel.h>

#if KICKOS_HAVE_ASPACE
#include <kickos/domain.h>
#include <kickos/sched.h>
#endif

namespace kickos
{
    namespace
    {
        KickosReentSeam s_seam = {};

        // Namespace scope and volatile: a local volatile would not stop the address being
        // materialised inline, which tests/static/check_riscv_kernel_apphalf.sh refuses.
        KickosReentSeam const* const volatile s_seam_home = &kickos_reent_seam;
    }

    void reent_seam_read(void)
    {
        // Through the kernel's own alias where the image is split: the descriptor is app-side
        // storage read before any address space exists.
        KickosReentSeam const* src = s_seam_home;
        KickosReentSeam const* const alias =
            static_cast<KickosReentSeam const*>(aspace_image_alias(s_seam_home));
        if (alias != nullptr)
        {
            src = alias;
        }
        s_seam = *src;

        // The out-of-range fallback in reent_state_for_slot ALIASES the process-wide state, so a
        // short descriptor would give two kernels' slots one errno with nothing to say so.
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
        size_t const index =
            static_cast<size_t>(kickos_instance_index()) * KICKOS_THREAD_SLOTS
            + static_cast<size_t>(slot);
        if (index >= static_cast<size_t>(s_seam.count))
        {
            return s_seam.shared;
        }
        return static_cast<unsigned char*>(s_seam.slots) + index * s_seam.stride;
    }

#if KICKOS_REENT_PER_THREAD and not KICKOS_REENT_IN_TCB
#if not KICKOS_HAVE_ASPACE
#error "the prime seats a per-thread element's id only through a translating backend"
#endif
    namespace
    {
        // noinline, and re-deriving what reent_prime was handed: anything reent_prime keeps
        // across its copy widens its frame on every interrupt route.
        __attribute__((noinline)) void prime_self(void)
        {
            Thread* const t = sched::current();
            uintptr_t const word = static_cast<uintptr_t>(t->lock_id);
            uintptr_t const at = reinterpret_cast<uintptr_t>(t->reent)
                + (reinterpret_cast<uintptr_t>(s_seam.self)
                   - reinterpret_cast<uintptr_t>(s_seam.slots));
            if (not kaccess_word_to_user(domain_space(thread_domain(t)), at, &word))
            {
                thread_cancel_escalate(t, CANCEL_SLAY);
            }
        }
    }
#endif

    void reent_prime(struct arch_aspace* space, void* state)
    {
        // s_seam.shared MUST stay pristine: every later prime copies it.
#if KICKOS_HAVE_ASPACE
        // ep_copy requires disjoint ends, which the slot array and the process-wide state are.
        if (not ep_copy(space, reinterpret_cast<uintptr_t>(state), space,
                        reinterpret_cast<uintptr_t>(s_seam.shared), s_seam.width))
        {
            thread_cancel_escalate(sched::current(), CANCEL_SLAY);
            return;
        }
#else
        (void)space;
        kmemcpy(state, s_seam.shared, s_seam.width);
#endif
#if KICKOS_REENT_PER_THREAD and not KICKOS_REENT_IN_TCB
        if (s_seam.self != nullptr)
        {
            prime_self();
        }
#endif
    }

#if KICKOS_REENT_IN_TCB
    void reent_seat_tcb(void* tls, void* state, uint32_t self)
    {
        // TCB words 0 and 1: the TLS payload starts KICKOS_ARCH_TLS_TCB above the thread pointer.
        static_assert(KICKOS_ARCH_TLS_TCB >= 2 * sizeof(void*),
                      "the control block holds the reent pointer and the lock id");
        void** const word = static_cast<void**>(__builtin_assume_aligned(tls, sizeof(void*)));
        uintptr_t const id = static_cast<uintptr_t>(self);
        __builtin_memcpy(word, &state, sizeof(state));
        __builtin_memcpy(word + 1, &id, sizeof(id));
    }
#endif
#if not KICKOS_REENT_PER_THREAD
    void reent_seat(struct arch_aspace* space, void* state, uint32_t self)
    {
        uintptr_t const id = static_cast<uintptr_t>(self);
#if KICKOS_HAVE_ASPACE
        // A silent refusal here leaves the seat word naming the outgoing thread's block, so
        // two processes resolve one errno and one stdio state through it.
        if (not kaccess_word_to_user(space, reinterpret_cast<uintptr_t>(s_seam.seat), &state)
            or not kaccess_word_to_user(space, reinterpret_cast<uintptr_t>(s_seam.self), &id))
        {
            thread_cancel_escalate(sched::current(), CANCEL_SLAY);
        }
#else
        // __builtin_memcpy: under -ffreestanding a plain memcpy lowers to a call on every switch.
        (void)space;
        void** const word =
            static_cast<void**>(__builtin_assume_aligned(s_seam.seat, sizeof(void*)));
        __builtin_memcpy(word, &state, sizeof(state));
        __builtin_memcpy(__builtin_assume_aligned(s_seam.self, sizeof(id)), &id, sizeof(id));
#endif
    }
#endif
}

#endif
