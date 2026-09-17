// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host address-space hooks for range validation. Removing a domain's space
// also removes its range list, matching domain_ranges.

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
            // Use a distinct address per simulated space.
            uint8_t g_spaces[FIXTURE_DOMAIN_SLOTS] = {};
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

    // No test creates CAP_ASPACE capabilities; all handles are invalid.
    Domain* domain_resolve(int)
    {
        return nullptr;
    }

    uint16_t domain_refcount(Domain const* d)
    {
        return testfix::domain_refs(d);
    }

    void ustack_free(Domain*, uintptr_t, size_t) {}

    struct arch_aspace* aspace_activate_for(Thread const*) { return nullptr; }

    bool aspace_seated_for(Thread const*)
    {
        return true;
    }

    void aspace_install_boot(void) {}

    void frame_pool_free_run(arch_phys_addr_t, size_t, size_t) {}
}

extern "C"
{
    // Provide runtime symbols required by KICKOS_HAVE_ASPACE.
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

    // Tests validate ranges without dereferencing them; acquisition must fail.
    void* arch_aspace_acquire(struct arch_aspace*, uintptr_t)
    {
        return nullptr;
    }

    void arch_aspace_release(struct arch_aspace*, uintptr_t) {}
}
