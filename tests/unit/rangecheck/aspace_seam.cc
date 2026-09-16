// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See aspace_seam.h. domain_ranges below answers exactly what kernel/domain/domain.cc
// answers, keyed on the space the domain holds, so an arm that takes the space away puts the
// checks through their no-list arm rather than a different one.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kickos/arch/arch.h>
#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/kernel.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/ustack.h>
#include <kickos/vrange.h>

#include "aspace_seam.h"
#include "kfixture.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            // Opaque to the kernel, so a distinct address per domain slot is a whole space.
            uint8_t g_spaces[KICKOS_MAX_TASKS] = {};
        }

        VirtualRanges* seat_space(Domain* d)
        {
            int const i = domain_index(d);
            if (i < 0)
            {
                return nullptr;
            }
            d->space = reinterpret_cast<struct arch_aspace*>(&g_spaces[i]);
            d->ranges.init(arch_aspace_granule());
            return &d->ranges;
        }

        void drop_space(Domain* d)
        {
            if (d != nullptr)
            {
                d->space = nullptr;
            }
        }

        void note_member_release() {}
    }

    struct arch_aspace* domain_space(Domain const* d)
    {
        if (d == nullptr)
        {
            return nullptr;
        }
        return d->space;
    }

    VirtualRanges const* domain_ranges(Domain const* d)
    {
        if (d == nullptr or d->space == nullptr)
        {
            return nullptr;
        }
        return &d->ranges;
    }

    // cap.cc's CAP_ASPACE arms resolve through these two. No arm here mints such a
    // capability, so a handle answering nothing is the right answer rather than an unreached
    // one.
    Domain* domain_resolve(int)
    {
        return nullptr;
    }

    uint16_t domain_refcount(Domain const* d)
    {
        return testfix::domain_refs(d);
    }

    void ustack_free(Domain*, uintptr_t, size_t) {}

    void aspace_activate_for(Thread const*) {}

    bool aspace_seated_for(Thread const*)
    {
        return true;
    }

    void aspace_install_boot(void) {}

    void frame_pool_free_run(arch_phys_addr_t, size_t, size_t) {}
}

extern "C"
{
    // KICKOS_HAVE_ASPACE turns the kmem* family from macros over libc into real symbols the
    // kernel image provides itself (kickos/kruntime.h), so this hosted program has to answer
    // for the ones the compiled sources reach.
    void* kmemset(void* dst, int c, size_t n)
    {
        return memset(dst, c, n);
    }

    void* kmemcpy(void* dst, void const* src, size_t n)
    {
        return memcpy(dst, src, n);
    }

    size_t arch_aspace_granule(void)
    {
        return 4096u;
    }

    // No arm here copies across a space: every range the checks answer is named and never
    // dereferenced, so a window that resolved would only hide a check that should have refused.
    void* arch_aspace_acquire(struct arch_aspace*, uintptr_t)
    {
        return nullptr;
    }

    void arch_aspace_release(struct arch_aspace*, uintptr_t) {}
}
