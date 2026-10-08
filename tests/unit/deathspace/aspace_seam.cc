// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See aspace_seam.h.

#include <stdint.h>
#include <string.h>

#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/kernel.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/ustack.h>

#include "aspace_seam.h"
#include "kfixture.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            // Use a distinct address per space, plus one for the boot root.
            uint8_t g_spaces[FIXTURE_DOMAIN_SLOTS + 1] = {};

            struct arch_aspace* g_installed[KICKOS_NUM_CORES] = {};

            struct arch_aspace* as(uint8_t* p)
            {
                return reinterpret_cast<struct arch_aspace*>(p);
            }

        }

        uint32_t g_ustack_frees = 0;
        uintptr_t g_ustack_free_base = 0;

        struct arch_aspace* boot_space()
        {
            return as(&g_spaces[FIXTURE_DOMAIN_SLOTS]);
        }

        struct arch_aspace* installed_on(uint32_t core)
        {
            if (core >= KICKOS_NUM_CORES)
            {
                return nullptr;
            }
            return g_installed[core];
        }

        struct arch_aspace* space_of(Domain const* d)
        {
            int const i = domain_index(d);
            if (i < 0)
            {
                return nullptr;
            }
            return as(&g_spaces[i]);
        }

        void install_here(struct arch_aspace* space)
        {
            uint32_t const core = arch_cpu_id();
            if (core < KICKOS_NUM_CORES)
            {
                g_installed[core] = space;
            }
            aspace_forget_current();
        }

        void note_member_release()
        {
            trace_add("task_release");
        }

        void aspace_seam_reset()
        {
#if KICKOS_NUM_CORES > 1
            uint32_t const was = g_core;
            for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
            {
                g_core = c;
                install_here(boot_space());
            }
            g_core = was;
#else
            install_here(boot_space());
#endif
            g_ustack_frees = 0;
            g_ustack_free_base = 0;
        }
    }

    struct arch_aspace* domain_space(Domain const* d)
    {
        return testfix::space_of(d);
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

    void ustack_free(Domain* d, uintptr_t base, size_t bytes)
    {
        // This maintenance requires the old root to remain installed.
        char const* seating = "unseated";
        if (testfix::installed_on(arch_cpu_id()) == testfix::space_of(d))
        {
            seating = "seated";
        }
        testfix::trace_add("ustack_free(%s)", seating);
        testfix::g_ustack_frees++;
        testfix::g_ustack_free_base = base;
        (void)bytes;
    }

    // No thread in these tests holds a window, so the exit path's unmap has nothing to take.
    VirtualRanges* domain_ranges_mut(Domain*)
    {
        return nullptr;
    }

    void frame_pool_free_run(arch_phys_addr_t, size_t, size_t) {}
}

extern "C"
{
    // kernel/mem/aspace.cc writes a core's root only when its own record of that core differs.
    void arch_aspace_activate(struct arch_aspace* space)
    {
        uint32_t const core = arch_cpu_id();
        if (core < KICKOS_NUM_CORES)
        {
            kickos::testfix::g_installed[core] = space;
        }
        if (space == kickos::testfix::boot_space())
        {
            kickos::testfix::trace_add("install_boot");
            return;
        }
        kickos::testfix::trace_add("activate");
    }

    struct arch_aspace* arch_aspace_boot(void)
    {
        return kickos::testfix::boot_space();
    }

    enum arch_aspace_result arch_aspace_unmap(struct arch_aspace*, uintptr_t, size_t)
    {
        return ARCH_ASPACE_OK;
    }

    void* kmemset(void* dst, int c, size_t n)
    {
        return memset(dst, c, n);
    }

    size_t arch_aspace_granule(void)
    {
        return 4096u;
    }
}
