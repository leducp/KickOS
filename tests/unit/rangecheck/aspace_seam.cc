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

        namespace
        {
            struct BackedPage
            {
                struct arch_aspace const* space;
                uintptr_t va;
                unsigned char* at;
                bool uncached;
            };

            BackedPage g_pages[BACKED_PAGES_MAX] = {};
            size_t g_page_count = 0;

            AcquireHolds g_holds;
            size_t g_past_min = 0;

            void* backed_at(struct arch_aspace const* space, uintptr_t va, bool* uncached)
            {
                uintptr_t const page = va & ~static_cast<uintptr_t>(arch_aspace_granule() - 1u);
                for (size_t i = 0; i < g_page_count; i++)
                {
                    BackedPage const& p = g_pages[i];
                    if (p.va == page and (p.space == nullptr or p.space == space))
                    {
                        if (uncached != nullptr)
                        {
                            *uncached = p.uncached;
                        }
                        return p.at + (va - page);
                    }
                }
                return nullptr;
            }
        }

        bool back_page(struct arch_aspace const* space, uintptr_t va, unsigned char* at,
                       bool uncached)
        {
            if (g_page_count == BACKED_PAGES_MAX
                or (va & static_cast<uintptr_t>(arch_aspace_granule() - 1u)) != 0)
            {
                return false;
            }
            g_pages[g_page_count] = BackedPage{space, va, at, uncached};
            g_page_count++;
            return true;
        }

        bool seat_backing(uintptr_t va, unsigned char* at, size_t bytes, uintptr_t uncached_va)
        {
            size_t const g = arch_aspace_granule();
            if ((va & static_cast<uintptr_t>(g - 1u)) != 0)
            {
                return false;
            }
            for (size_t off = 0; off < bytes; off += g)
            {
                if (not back_page(nullptr, va + off, at + off, va + off >= uncached_va))
                {
                    return false;
                }
            }
            return true;
        }

        void unback_all()
        {
            g_page_count = 0;
            g_holds = AcquireHolds{};
            g_past_min = 0;
        }

        AcquireHolds const& holds()
        {
            return g_holds;
        }

        size_t holds_refused_past_min()
        {
            return g_past_min;
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

    void frame_pool_free_run(arch_phys_addr_t, size_t, size_t) {}
}

extern "C"
{
    void arch_aspace_activate(struct arch_aspace*) {}
    struct arch_aspace* arch_aspace_boot(void) { return nullptr; }
    enum arch_aspace_result arch_aspace_unmap(struct arch_aspace*, uintptr_t, size_t)
    {
        return ARCH_ASPACE_OK;
    }

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

    // Fails outside the backed pages, so a range check never dereferences. Refuses a hold past
    // ARCH_ASPACE_ACQUIRE_MIN, as a windowed backend out of slots does.
    void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va, bool* uncached)
    {
        if (space == nullptr)
        {
            return nullptr;
        }
        if (kickos::testfix::g_holds.live >= ARCH_ASPACE_ACQUIRE_MIN)
        {
            kickos::testfix::g_past_min++;
            return nullptr;
        }
        void* const p = kickos::testfix::backed_at(space, va, uncached);
        if (p != nullptr)
        {
            kickos::testfix::g_holds.take(space, va);
        }
        return p;
    }

    void arch_aspace_release(struct arch_aspace* space, uintptr_t va)
    {
        kickos::testfix::g_holds.give(space, va);
    }
}
