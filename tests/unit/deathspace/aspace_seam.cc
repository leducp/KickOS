// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See aspace_seam.h. Every definition here stands in for a kernel/mem translation unit the
// host arch has no board for; each one that exit_current reaches writes the ordered trace,
// because the claim this gate carries is about ORDER.

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
            // Opaque to the kernel, so a distinct address per slot is a whole space. One extra
            // for the boot root.
            uint8_t g_spaces[KICKOS_MAX_TASKS + 1] = {};

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
            return as(&g_spaces[KICKOS_MAX_TASKS]);
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
        }

        void note_member_release()
        {
            trace_add("task_release");
        }

        void aspace_seam_reset()
        {
            for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
            {
                g_installed[c] = boot_space();
            }
            g_ustack_frees = 0;
            g_ustack_free_base = 0;
        }
    }

    struct arch_aspace* domain_space(Domain const* d)
    {
        return testfix::space_of(d);
    }

    // cap.cc's CAP_ASPACE arms resolve through these two. No arm here mints such a capability,
    // so a handle answering nothing is the right answer rather than an unreached one.
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
        // The space must still be installed here: the maintenance this call pays on a
        // translating backend is against the running root, which is why the install below
        // follows it rather than preceding it.
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

    void aspace_activate_for(Thread const* t)
    {
        if (t == nullptr)
        {
            return;
        }
        struct arch_aspace* space = domain_space(task_domain(t->task));
        if (space == nullptr)
        {
            space = testfix::boot_space();
        }
        if (testfix::installed_on(arch_cpu_id()) == space)
        {
            return;
        }
        testfix::install_here(space);
        testfix::trace_add("activate_for(t%u)", static_cast<unsigned>(t->id));
    }

    bool aspace_seated_for(Thread const* t)
    {
        if (t == nullptr)
        {
            return false;
        }
        struct arch_aspace* const space = domain_space(task_domain(t->task));
        if (space == nullptr)
        {
            return false;
        }
        return space == testfix::installed_on(arch_cpu_id());
    }

    void aspace_install_boot(void)
    {
        testfix::trace_add("install_boot");
        testfix::install_here(testfix::boot_space());
    }

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

    size_t arch_aspace_granule(void)
    {
        return 4096u;
    }
}
