// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/domain.h>

#include <kickos/aspace.h> // aspace_image_seed / aspace_release
#include <kickos/grant.h> // grant_nocache_admissible
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/debug.h>  // KICKOS_DEBUG_ASSERT
#include <kickos/kernel.h> // KICKOS_ASSERT
#include <kickos/klink.h>
#include <kickos/slotpool.h> // handle_pack / handle_index / handle_gen

#include <kickos/sys/errno.h>

#include <stdint.h> // UINT16_MAX

namespace kickos
{
    namespace
    {
        // The two immortal domains are pinned to the first pool slots; free_slot() never
        // returns them.
        enum { KDOM_KERNEL_INDEX = 0, KDOM_DEFAULT_USER_INDEX = 1 };

        static_assert(KICKOS_MAX_DOMAINS > KDOM_DEFAULT_USER_INDEX + 1,
                      "the domain pool must seat both immortal singletons and still leave a "
                      "slot free_slot can hand out");

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

#if KICKOS_HAVE_ASPACE
        // The edge is returned to the caller; a release here would recurse down a chain of
        // borrowers.
        Domain* drop_space(Domain* d)
        {
            Domain* const donor = d->borrowed_from;
            d->borrowed_from = nullptr;
            if (d->space != nullptr)
            {
                aspace_release(d->space, &d->ranges);
                d->space = nullptr;
            }
            return donor;
        }
#endif

        // A fresh unprivileged domain, with the address space it needs where a backend
        // translates, seeded for `caller`'s posture. On refusal the slot is left reinitialised
        // and *err written.
        Domain* claim_slot(uint32_t caller, Domain const* donor, int* err)
        {
#if not KICKOS_HAVE_ASPACE
            (void)caller;
            (void)donor;
#endif
            Domain* d = free_slot();
            if (d == nullptr)
            {
                *err = KOS_ENOMEM;
                return nullptr;
            }
#if KICKOS_HAVE_ASPACE
            // A free slot can still hold a space: reusing one still carrying a root would drop
            // the only handle to it, its tables and the donor edge.
            Domain* const stale_donor = drop_space(d);
#endif
            // Survives the reinitialisation and advances with it, or a capability naming this
            // slot's previous occupant resolves to the new one.
            uint16_t const gen = static_cast<uint16_t>(d->generation + 1u);
            *d = Domain{};
            d->generation = gen;
#if KICKOS_HAVE_ASPACE
            // After the reinitialisation, so a cascade that frees further slots cannot find
            // this one half written.
            domain_release(stale_donor);
            d->space = arch_aspace_create();
            if (d->space == nullptr)
            {
                *err = KOS_ENOMEM;
                return nullptr;
            }
            struct arch_aspace* spawner = nullptr;
            if (donor != nullptr)
            {
                spawner = donor->space;
            }
            bool const from_snapshot = (caller & DOM_CALLER_TASK) != 0;
            if (not aspace_image_seed(d->space, &d->ranges, from_snapshot, spawner))
            {
                (void)drop_space(d); // a space with no handoff yet: the edge is null
                *err = KOS_ENOMEM;
                return nullptr;
            }
#endif
            return d;
        }
    }

    void domain_init(void)
    {
        Kernel& k = kernel();
        for (int i = 0; i < KICKOS_MAX_DOMAINS; i++)
        {
            k.domains[i] = Domain{};
        }
        Domain* const kdom = &k.domains[KDOM_KERNEL_INDEX];
        kdom->privileged = true;
        kdom->immortal = true;
        size_t size = arch_ram_size();
        if (size != 0)
        {
            kdom->regions[0].base = arch_ram_base();
            kdom->regions[0].size = size;
            kdom->regions[0].attr = ARCH_MPU_R | ARCH_MPU_W;
            kdom->region_count = 1;
        }
        k.domains[KDOM_DEFAULT_USER_INDEX].immortal = true;
    }

    // Only domain_init and domain_for may touch regions[] directly; every reader outside this
    // file comes through these accessors.
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
        KICKOS_DEBUG_ASSERT(i < KICKOS_DOMAIN_REGIONS);
        return &d->regions[i];
    }

    Domain* domain_kernel(void)
    {
        return &kernel().domains[KDOM_KERNEL_INDEX];
    }

    Domain* domain_default_user(void)
    {
        return &kernel().domains[KDOM_DEFAULT_USER_INDEX];
    }

    // SlotPool's handle codec. An aged handle is negative, so no decoder may test its sign; -1,
    // the null domain's handle, is refused by its all-ones index.
    static_assert(KICKOS_MAX_DOMAINS < HANDLE_INDEX_MASK, "a domain index must never be all ones");

    int domain_handle(Domain const* d)
    {
        if (d == nullptr)
        {
            return -1;
        }
        size_t const idx = static_cast<size_t>(d - &kernel().domains[0]);
        return static_cast<int>(handle_pack(d->generation, static_cast<uint32_t>(idx)));
    }

    Domain* domain_resolve(int handle)
    {
        uint32_t const raw = static_cast<uint32_t>(handle);
        size_t const idx = handle_index(raw);
        if (idx >= KICKOS_MAX_DOMAINS)
        {
            return nullptr;
        }
        Domain* d = &kernel().domains[idx];
        if (d->generation != handle_gen(raw))
        {
            return nullptr; // the slot has been reclaimed since this handle was minted
        }
        if (d->refcount == 0 and not d->immortal)
        {
            return nullptr; // free slot: a live handle to one cannot exist
        }
        return d;
    }

    struct arch_aspace* domain_space(Domain const* d)
    {
#if KICKOS_HAVE_ASPACE
        if (d == nullptr)
        {
            return nullptr;
        }
        return d->space;
#else
        (void)d;
        return nullptr;
#endif
    }

#if KICKOS_HAVE_ASPACE
    VirtualRanges const* domain_ranges(Domain const* d)
    {
        if (d == nullptr or d->space == nullptr)
        {
            return nullptr;
        }
        return &d->ranges;
    }

    VirtualRanges* domain_ranges_mut(Domain* d)
    {
        return const_cast<VirtualRanges*>(domain_ranges(d));
    }

    void domain_retire_handles(Domain* d)
    {
        if (d != nullptr and not d->immortal)
        {
            d->generation++;
        }
    }

    void domain_retire_space(Domain* d)
    {
        if (d != nullptr)
        {
            aspace_release_runs(d->space, &d->ranges);
        }
    }

    size_t domain_spaces_held(void)
    {
        Kernel& k = kernel();
        size_t held = 0;
        for (int i = 0; i < KICKOS_MAX_DOMAINS; i++)
        {
            if (k.domains[i].space != nullptr)
            {
                held++;
            }
        }
        return held;
    }
#endif

    Domain* domain_for(uint32_t caller, void* mem_base, size_t mem_size, uint32_t mem_attr,
                       Domain* donor, int* err)
    {
        *err = 0;
        if ((caller & DOM_CALLER_PRIVILEGED) != 0)
        {
            return domain_kernel();
        }
        if (mem_base == nullptr or mem_size == 0)
        {
#if KICKOS_HAVE_ASPACE
            return claim_slot(caller, donor, err);
#else
            return domain_default_user();
#endif
        }
        uintptr_t const base = reinterpret_cast<uintptr_t>(mem_base);
        // Access is the grant's own; only the memory-type bits come from the caller.
        uint32_t const attr = ARCH_MPU_R | ARCH_MPU_W | (mem_attr & ARCH_MPU_NOCACHE);
        if (not grant_nocache_admissible(attr))
        {
            *err = KOS_ENOTSUP;
            return nullptr;
        }
        Domain* d = claim_slot(caller, donor, err);
        if (d == nullptr)
        {
            return nullptr;
        }
#if KICKOS_HAVE_ASPACE
        // On a translating backend admission is this handoff: the range must be one the donor
        // reserved, which no MMIO block and no kernel address can be. A refusal leaves no
        // half-built domain: the release below frees the space and the slot.
        enum arch_map_memtype mtype = ARCH_MAP_NORMAL;
        if ((mem_attr & ARCH_MPU_NOCACHE) != 0)
        {
            mtype = ARCH_MAP_NOCACHE;
        }
        int const hrc = aspace_handoff(domain_ranges(donor), d->space, &d->ranges, base,
                                       mem_size, mtype);
        if (hrc != 0)
        {
            domain_release(d);
            *err = -hrc;
            return nullptr;
        }
        // The lifetime edge, and the only place one is made. Without it the donor's last task
        // can exit, destroy its space and return every borrowed frame to the pool under a live
        // mapping. Taken only on success, so aspace_handoff's unwind arms surrender nothing.
        KICKOS_DEBUG_ASSERT(d->borrowed_from == nullptr and d != donor);
        d->borrowed_from = donor;
        domain_ref(donor);
        // The range list carries the handoff with the exact extent, and is what the entry path
        // answers from on this backend.
#else
        d->regions[0].base = base;
        d->regions[0].size = arch_ram_region_size(mem_size);
        d->regions[0].attr = attr;
        d->region_count = 1;
#endif
        return d;
    }

    static_assert(2ull * KICKOS_MAX_TASKS + KICKOS_MAX_DOMAINS <= UINT16_MAX,
                  "Domain::refcount is uint16_t and counts live tasks, creator holds and "
                  "borrowers: twice the task pool plus the domain pool must fit it");

    uint16_t domain_refcount(Domain const* d)
    {
        if (d == nullptr)
        {
            return 0;
        }
        return d->refcount;
    }

    void domain_ref(Domain* d)
    {
        // An immortal domain's refcount is not tracked: an unbounded, transient set of tasks
        // references it and the counter would wrap.
        if (d != nullptr and not d->immortal)
        {
            KICKOS_DEBUG_ASSERT(d->refcount < UINT16_MAX);
            d->refcount++;
        }
    }

    void domain_release(Domain* d)
    {
        // Iterative: freeing a borrower surrenders its donor's reference, which can free the
        // donor in turn, and the chain runs as long as the domain pool, with a tree walk a link.
        while (d != nullptr and not d->immortal)
        {
            if (d->refcount > 0)
            {
                d->refcount--;
            }
            if (d->refcount != 0)
            {
                return;
            }
#if KICKOS_HAVE_ASPACE
            // drop_space hands back the donor whose reference the next turn of the loop drops;
            // reusing the slot without it would strand the root and its tables.
            d = drop_space(d);
#else
            return;
#endif
        }
    }
}

namespace
{
    // end > start, never start != 0: a zero start is valid on flash-at-zero targets.
    void add_static(struct arch_mpu_region* out, size_t* n, size_t max, unsigned char const* start,
                    unsigned char const* end, uint32_t attr)
    {
        uintptr_t const lo = reinterpret_cast<uintptr_t>(start);
        uintptr_t const hi = reinterpret_cast<uintptr_t>(end);
        if (hi > lo and *n < max)
        {
            out[*n].base = lo;
            out[*n].size = static_cast<size_t>(hi - lo);
            out[*n].attr = attr;
            (*n)++;
        }
    }
}

extern "C"
{
    extern unsigned char __kickos_code_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_code_end[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_appdata_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_appdata_end[] KICKOS_LINK_BOUND;

    size_t arch_domain_static_regions(struct arch_mpu_region* out, size_t max)
    {
        size_t n = 0;
        add_static(out, &n, max, __kickos_code_start, __kickos_code_end, ARCH_MPU_R | ARCH_MPU_X);
        add_static(out, &n, max, __kickos_appdata_start, __kickos_appdata_end,
                   ARCH_MPU_R | ARCH_MPU_W);
        return n;
    }
}
