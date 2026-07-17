// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/domain.h>

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h> // KICKOS_ASSERT

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

    Domain* domain_for(bool privileged, void* mem_base, size_t mem_size,
                       void* mmio_base, size_t mmio_size)
    {
        if (privileged)
        {
            return g_kernel;
        }
        bool const has_data = (mem_base != nullptr and mem_size != 0);
        bool const has_mmio = (mmio_base != nullptr and mmio_size != 0);
        if (not has_data and not has_mmio)
        {
            return g_default_user;
        }
        Kernel& k = kernel();
        uintptr_t const base = reinterpret_cast<uintptr_t>(mem_base);
        size_t const rsz = arch_ram_region_size(mem_size);
        // Share: a live unprivileged data-ONLY domain describing exactly this one
        // region is the same domain (the documented "threads sharing one region share
        // a domain"). Match the rounded size so a re-grant of the same block dedups.
        // An MMIO grant is a capability -- never shared -- so an MMIO-carrying spawn
        // skips this and always takes a fresh slot; the attr guard also stops a
        // data-only spawn from ever landing on an existing MMIO (DEV) domain.
        if (not has_mmio)
        {
            for (int i = 0; i < KICKOS_MAX_DOMAINS; i++)
            {
                Domain& d = k.domains[i];
                if (d.refcount > 0 and not d.privileged and d.region_count == 1
                    and d.regions[0].base == base and d.regions[0].size == rsz
                    and d.regions[0].attr == (ARCH_MPU_R | ARCH_MPU_W))
                {
                    return &d;
                }
            }
        }
        Domain* d = free_slot();
        if (d == nullptr)
        {
            return nullptr; // pool exhausted -- the caller fails the spawn
        }
        *d = Domain{};
        d->privileged = false;
        size_t n = 0;
        if (has_data)
        {
            d->regions[n].base = base;
            d->regions[n].size = rsz;
            d->regions[n].attr = ARCH_MPU_R | ARCH_MPU_W;
            n++;
        }
        if (has_mmio)
        {
            // Exact window: validated encodable at the spawn boundary, never rounded
            // (rounding would over-grant the neighbouring registers). Fixed R|W|DEV,
            // never executable.
            d->regions[n].base = reinterpret_cast<uintptr_t>(mmio_base);
            d->regions[n].size = mmio_size;
            d->regions[n].attr = ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV;
            KICKOS_ASSERT((d->regions[n].attr & ARCH_MPU_X) == 0); // MMIO is never executable
            n++;
        }
        d->region_count = n;
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

// arch_domain_static_regions -- arch-independent: reads the linker-defined app
// code (RX) + static data/.bss (RW-NX) sections into the regions every
// UNPRIVILEGED thread needs to run under MPU enforcement. WEAK symbols: a chip
// whose linker script does not carve them (no-MPU parts, the host sim) leaves
// both start and end at 0, so `end > start` is false and no region is emitted.
// That same test is why there is NO `start != 0` guard: a flash-at-0 chip (K64F:
// __kickos_code_start == ORIGIN(FLASH) == 0) has 0 as a VALID base, and a != 0
// sentinel would wrongly read it as "absent" and drop the code region -- faulting
// the thread on its first instruction fetch. C linkage matches the arch.h decl.
extern "C"
{
    extern unsigned char __kickos_code_start[] __attribute__((weak));
    extern unsigned char __kickos_code_end[] __attribute__((weak));
    extern unsigned char __kickos_appdata_start[] __attribute__((weak));
    extern unsigned char __kickos_appdata_end[] __attribute__((weak));

    size_t arch_domain_static_regions(struct arch_mpu_region* out, size_t max)
    {
        // Decay the linker-symbol arrays to uintptr_t before comparing: a direct
        // `end > start` on two array-typed externs trips -Warray-compare (gcc 12+).
        size_t n = 0;
        uintptr_t const code_start = reinterpret_cast<uintptr_t>(__kickos_code_start);
        uintptr_t const code_end = reinterpret_cast<uintptr_t>(__kickos_code_end);
        if (code_end > code_start and n < max)
        {
            out[n].base = code_start;
            out[n].size = static_cast<size_t>(code_end - code_start);
            out[n].attr = ARCH_MPU_R | ARCH_MPU_X; // code: read + execute, no write
            n++;
        }
        uintptr_t const data_start = reinterpret_cast<uintptr_t>(__kickos_appdata_start);
        uintptr_t const data_end = reinterpret_cast<uintptr_t>(__kickos_appdata_end);
        if (data_end > data_start and n < max)
        {
            out[n].base = data_start;
            out[n].size = static_cast<size_t>(data_end - data_start);
            out[n].attr = ARCH_MPU_R | ARCH_MPU_W; // static data/.bss: read + write, no-execute
            n++;
        }
        return n;
    }
}
