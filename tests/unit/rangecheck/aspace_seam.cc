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

        Backing g_backing = {};

        void seat_backing(uintptr_t va, unsigned char* at, size_t bytes, uintptr_t uncached_va)
        {
            g_backing.va = va;
            g_backing.at = at;
            g_backing.bytes = bytes;
            g_backing.uncached_va = uncached_va;
        }
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

    // No thread in these tests holds a window.
    VirtualRanges* domain_ranges_mut(Domain*)
    {
        return nullptr;
    }

    void aspace_window_unmap_holder(struct arch_aspace*, VirtualRanges*, uint16_t) {}

    // The exit path's end of a call that syncs ahead of its lock; no arm here makes one.
    void presync_exit() {}

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

    uintptr_t arch_aspace_user_offset(void)
    {
        return 0;
    }

    // Fails outside the window seat_backing names, so a range check never dereferences.
    void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va, bool* uncached)
    {
        using kickos::testfix::g_backing;
        if (space == nullptr or g_backing.bytes == 0 or va < g_backing.va
            or va - g_backing.va >= g_backing.bytes)
        {
            return nullptr;
        }
        if (uncached != nullptr)
        {
            *uncached = va >= g_backing.uncached_va;
        }
        return g_backing.at + (va - g_backing.va);
    }

    void arch_aspace_release(struct arch_aspace*, uintptr_t) {}
}
