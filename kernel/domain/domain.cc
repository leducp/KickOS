// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/domain.h>

#include <kickos/instance.h>
#include <kickos/irqlock.h>

namespace kickos
{
    namespace
    {
        Domain* g_kernel = nullptr;       // domains[0]
        Domain* g_default_user = nullptr; // domains[1]

        // A slot is free iff it is not immortal and holds no live thread.
        Domain* free_slot()
        {
            Kernel& k = kernel();
            for (int i = 0; i < KICKOS_MAX_DOMAINS; i++)
            {
                Domain& d = k.domains[i];
                if (not d.immortal and d.refcount == 0)
                {
                    return &d;
                }
            }
            return nullptr;
        }
    }

    void domain_init(void)
    {
        Kernel& k = kernel();
        for (int i = 0; i < KICKOS_MAX_DOMAINS; i++)
        {
            k.domains[i] = Domain{};
        }
        // Kernel domain: the whole user-RAM arena, privileged (the background-region
        // analog). Immortal -- root/idle and every privileged thread reference it.
        g_kernel = &k.domains[0];
        g_kernel->privileged = true;
        g_kernel->immortal = true;
        size_t size = arch_ram_size();
        if (size != 0)
        {
            g_kernel->regions[0].base = arch_ram_base();
            g_kernel->regions[0].size = size;
            g_kernel->regions[0].attr = ARCH_MPU_R | ARCH_MPU_W;
            g_kernel->region_count = 1;
        }
        // Default user domain: no granted arena region, unprivileged. Immortal --
        // every unprivileged thread with no explicit grant shares it (the app
        // domain; its code/data are ungoverned on the sim, real regions on HW).
        g_default_user = &k.domains[1];
        g_default_user->privileged = false;
        g_default_user->immortal = true;
        g_default_user->region_count = 0;
    }

    Domain* domain_kernel(void)
    {
        return g_kernel;
    }

    Domain* domain_default_user(void)
    {
        return g_default_user;
    }

    Domain* domain_for(bool privileged, void* mem_base, size_t mem_size)
    {
        if (privileged)
        {
            return g_kernel;
        }
        if (mem_base == nullptr or mem_size == 0)
        {
            return g_default_user;
        }
        Kernel& k = kernel();
        uintptr_t const base = reinterpret_cast<uintptr_t>(mem_base);
        size_t const rsz = arch_ram_region_size(mem_size);
        // Share: a live unprivileged domain describing exactly this one region is
        // the same domain (the documented "threads sharing one region share a
        // domain"). Match the rounded size so a re-grant of the same block dedups.
        for (int i = 0; i < KICKOS_MAX_DOMAINS; i++)
        {
            Domain& d = k.domains[i];
            if (d.refcount > 0 and not d.privileged and d.region_count == 1
                and d.regions[0].base == base and d.regions[0].size == rsz)
            {
                return &d;
            }
        }
        Domain* d = free_slot();
        if (d == nullptr)
        {
            return nullptr; // pool exhausted -- the caller fails the spawn
        }
        *d = Domain{};
        d->privileged = false;
        d->regions[0].base = base;
        d->regions[0].size = rsz;
        d->regions[0].attr = ARCH_MPU_R | ARCH_MPU_W;
        d->region_count = 1;
        return d;
    }

    void domain_ref(Domain* d)
    {
        // Immortal domains are referenced by an unbounded, transient set of threads,
        // so their refcount is meaningless (they never free); skip it to avoid a wrap.
        if (d != nullptr and not d->immortal)
        {
            d->refcount++;
        }
    }

    void domain_release(Domain* d)
    {
        // Immortal domains never free; refcount is not tracked for them (they are
        // referenced by an unbounded, transient set of threads). A non-immortal
        // domain returns to the pool when its last thread leaves (refcount 0).
        if (d == nullptr or d->immortal)
        {
            return;
        }
        if (d->refcount > 0)
        {
            d->refcount--;
        }
    }
}
