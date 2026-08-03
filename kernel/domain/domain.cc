// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/domain.h>

#include <kickos/grant.h> // grant_region_admissible (Rule 7 chokepoint)
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/debug.h>  // KICKOS_DEBUG_ASSERT
#include <kickos/kernel.h> // KICKOS_ASSERT

#include <kickos/sys/errno.h>

#include <stdint.h> // UINT16_MAX

namespace kickos
{
    namespace
    {
        // The two immortal domains are pinned to the first pool slots and are never
        // returned by free_slot().
        enum { KDOM_KERNEL_INDEX = 0, KDOM_DEFAULT_USER_INDEX = 1 };

        Domain* g_kernel = nullptr;       // domains[KDOM_KERNEL_INDEX]
        Domain* g_default_user = nullptr; // domains[KDOM_DEFAULT_USER_INDEX]

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

    bool dev_window_free(uintptr_t base, size_t size)
    {
        uintptr_t const last = base + size - 1u;
        Kernel& k = kernel();
        for (int i = 0; i < KICKOS_MAX_DOMAINS; i++)
        {
            Domain const& d = k.domains[i];
            if (not d.immortal and d.refcount == 0)
            {
                continue; // free slot / rollback debris
            }
            size_t const n = domain_region_count(&d);
            for (size_t r = 0; r < n; r++)
            {
                arch_mpu_region const* reg = domain_region_at(&d, r);
                if ((reg->attr & ARCH_MPU_DEV) == 0)
                {
                    continue;
                }
                if (grant_ranges_overlap(base, last, reg->base,
                                         reg->base + reg->size - 1u))
                {
                    return false;
                }
            }
        }
        return true;
    }

    void domain_init(void)
    {
        Kernel& k = kernel();
        for (int i = 0; i < KICKOS_MAX_DOMAINS; i++)
        {
            k.domains[i] = Domain{};
        }
        // Kernel domain: the whole user-RAM arena, privileged. Immortal because
        // root, idle and every privileged thread reference it.
        g_kernel = &k.domains[KDOM_KERNEL_INDEX];
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
        // Default user domain: no granted arena region, unprivileged. Immortal because
        // every unprivileged thread with no explicit grant shares it.
        g_default_user = &k.domains[KDOM_DEFAULT_USER_INDEX];
        g_default_user->privileged = false;
        g_default_user->immortal = true;
        g_default_user->region_count = 0;
    }

    // Only domain_init and domain_for below may touch regions[] directly. Every reader
    // outside this file MUST come through these accessors, so the representation stays
    // changeable.
    size_t domain_region_count(Domain const* d)
    {
        if (d == nullptr)
        {
            return 0;
        }
        return d->region_count;
    }

    arch_mpu_region const* domain_region_at(Domain const* d, size_t i)
    {
        return &d->regions[i];
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
                       void* mmio_base, size_t mmio_size, bool caller_authorized,
                       int* err)
    {
        *err = 0;
        if (privileged)
        {
            return g_kernel;
        }
        bool const has_data = (mem_base != nullptr and mem_size != 0);
        // A non-null MMIO base with size 0 is malformed, NOT "no MMIO". Refused here as
        // well as at the spawn boundary so domain_for stays a complete chokepoint for a
        // caller that skips the boundary.
        if (mmio_base != nullptr and mmio_size == 0)
        {
            *err = KOS_EINVAL; // malformed window: a base with no extent
            return nullptr;
        }
        bool const has_mmio = (mmio_base != nullptr and mmio_size != 0);
        if (not has_data and not has_mmio)
        {
            return g_default_user;
        }
        // Rule 7 admits the PROSPECTIVE COMMITTED geometry, and must run before a slot is
        // allocated so a refusal is a clean spawn failure, not a half-built domain.
        if (has_data)
        {
            uintptr_t const db = reinterpret_cast<uintptr_t>(mem_base);
            if (not grant_region_admissible(db, arch_ram_region_size(mem_size),
                                            ARCH_MPU_R | ARCH_MPU_W, caller_authorized))
            {
                *err = KOS_EPERM; // out-of-arena / reserved-block hit: never admissible
                return nullptr;
            }
        }
        if (has_mmio)
        {
            uintptr_t const mb = reinterpret_cast<uintptr_t>(mmio_base);
            if (not grant_region_admissible(mb, mmio_size,
                                            ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV,
                                            caller_authorized))
            {
                *err = KOS_EPERM; // reserved block / bit-band alias / unauthorized DEV
                return nullptr;
            }
            // ONE HOLDER PER DEVICE WINDOW. Matched on RANGES, not on slots: an encodable
            // window can span several peripheral sub-units or cover part of one, so
            // equal, containing and straddling requests must all refuse while an ADJACENT
            // window stays admissible. "Live" is the inverse of free_slot: an immortal
            // domain's refcount is not tracked and must not be read as free, while a
            // refcount-0 mortal domain is rollback debris from a spawn that failed after
            // domain_for and is never a holder, so a retry is not self-blocked.
            if (not dev_window_free(mb, mmio_size))
            {
                *err = KOS_EBUSY; // already held; no stealing
                return nullptr;
            }
        }
        Kernel& k = kernel();
        uintptr_t const base = reinterpret_cast<uintptr_t>(mem_base);
        size_t const rsz = arch_ram_region_size(mem_size);
        // Threads sharing one region share a domain, so a live unprivileged data-ONLY
        // domain describing exactly this region is reused. The match is on the ROUNDED
        // size, so a re-grant of the same block dedups. An MMIO grant is a capability and
        // is never shared: an MMIO-carrying spawn always takes a fresh slot, which is what
        // makes one grant == one domain == one thread and the dev_window_free scan above
        // exact. The attr test also keeps a data-only spawn off an existing DEV domain.
        if (not has_mmio)
        {
            for (int i = 0; i < KICKOS_MAX_DOMAINS; i++)
            {
                Domain& d = k.domains[i];
                if (d.refcount > 0 and not d.privileged and domain_region_count(&d) == 1)
                {
                    arch_mpu_region const* r0 = domain_region_at(&d, 0);
                    if (r0->base == base and r0->size == rsz
                        and r0->attr == (ARCH_MPU_R | ARCH_MPU_W))
                    {
                        return &d;
                    }
                }
            }
        }
        Domain* d = free_slot();
        if (d == nullptr)
        {
            *err = KOS_ENOMEM; // pool exhausted: retry later
            return nullptr;
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
            // The exact window, validated encodable at the spawn boundary. NEVER rounded:
            // rounding would over-grant the neighbouring registers.
            d->regions[n].base = reinterpret_cast<uintptr_t>(mmio_base);
            d->regions[n].size = mmio_size;
            d->regions[n].attr = ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV;
            KICKOS_ASSERT((d->regions[n].attr & ARCH_MPU_X) == 0);
            n++;
        }
        d->region_count = n;
        return d;
    }

    // A mortal domain's refcount counts live threads and nothing else: thread_create takes
    // the single reference, sched::exit_current drops it. The bound is the thread-handle
    // index field, NOT KICKOS_MAX_THREADS.
    static_assert((1ull << ThreadPool::INDEX_BITS) - 1ull <= UINT16_MAX,
                  "Domain::refcount is uint16_t and counts live threads: the thread pool "
                  "ceiling (1 << ThreadPool::INDEX_BITS) must fit it");

    void domain_ref(Domain* d)
    {
        // An immortal domain's refcount is deliberately NOT tracked: an unbounded,
        // transient set of threads references it and the counter would wrap.
        if (d != nullptr and not d->immortal)
        {
            // Trips on a reference held by something other than a live thread.
            KICKOS_DEBUG_ASSERT(d->refcount < KICKOS_MAX_THREADS);
            d->refcount++;
        }
    }

    void domain_release(Domain* d)
    {
        // A mortal domain returns to the pool at refcount 0. Immortal ones never free and
        // never count.
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

// Reads the linker-defined app code (RX) and static data/.bss (RW-NX) sections into the
// regions every UNPRIVILEGED thread needs under MPU enforcement. The symbols are WEAK: a
// chip whose linker script does not carve them leaves both start and end at 0, so
// `end > start` is false and no region is emitted. That is also why there is NO
// `start != 0` guard: on a flash-at-0 chip (K64F, __kickos_code_start == 0) that sentinel
// would read a VALID base as absent, drop the code region, and fault the thread on its
// first instruction fetch.
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
            out[n].attr = ARCH_MPU_R | ARCH_MPU_X;
            n++;
        }
        uintptr_t const data_start = reinterpret_cast<uintptr_t>(__kickos_appdata_start);
        uintptr_t const data_end = reinterpret_cast<uintptr_t>(__kickos_appdata_end);
        if (data_end > data_start and n < max)
        {
            out[n].base = data_start;
            out[n].size = static_cast<size_t>(data_end - data_start);
            out[n].attr = ARCH_MPU_R | ARCH_MPU_W;
            n++;
        }
        return n;
    }
}
