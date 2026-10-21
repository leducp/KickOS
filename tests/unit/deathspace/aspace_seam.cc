// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See aspace_seam.h.

#include <stdint.h>

#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/irqlock.h>
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

        void install_boot_here()
        {
            IrqLock lock;
            aspace_install_boot();
        }

        void install_for(Thread const* t)
        {
            IrqLock lock;
            (void)aspace_activate_for(t);
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
                install_boot_here();
            }
            g_core = was;
#else
            install_boot_here();
#endif
            g_ustack_frees = 0;
            g_ustack_free_base = 0;
        }
    }

    struct arch_aspace* domain_space(Domain const* d)
    {
        return testfix::space_of(d);
    }

    // No windows at this posture: nothing for a death to unmap.
    VirtualRanges const* domain_ranges(Domain const*)
    {
        return nullptr;
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
}
