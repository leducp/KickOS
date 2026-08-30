// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/domain.h>

#include <kickos/aspace.h> // aspace_image_seed / aspace_release
#include <kickos/grant.h> // grant_region_admissible
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/debug.h>  // KICKOS_DEBUG_ASSERT
#include <kickos/kernel.h> // KICKOS_ASSERT
#include <kickos/klink.h>

#include <kickos/sys/errno.h>

#include <stdint.h> // UINT16_MAX

namespace kickos
{
    namespace
    {
        // The two immortal domains are pinned to the first pool slots; free_slot() never
        // returns them.
        enum { KDOM_KERNEL_INDEX = 0, KDOM_DEFAULT_USER_INDEX = 1 };

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
        // The only caller of aspace_release in this file, so no site can unmap the borrowed
        // entry without surrendering the reference it rests on. The edge is returned to the
        // caller; a release here would recurse down a chain of borrowers.
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
        // translates. On refusal the slot is left reinitialised and *err written.
        Domain* claim_slot(int* err)
        {
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
            d->privileged = false;
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
            if (not aspace_image_seed(d->space, &d->ranges))
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
        Domain* const udom = &k.domains[KDOM_DEFAULT_USER_INDEX];
        udom->privileged = false;
        udom->immortal = true;
        udom->region_count = 0;
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

    // Same codec as SlotPool's handle: generation in the high half, slot index in the low.
    int domain_handle(Domain const* d)
    {
        if (d == nullptr)
        {
            return -1;
        }
        size_t const idx = static_cast<size_t>(d - &kernel().domains[0]);
        return static_cast<int>((static_cast<uint32_t>(d->generation) << 16)
                                | static_cast<uint32_t>(idx));
    }

    Domain* domain_resolve(int handle)
    {
        if (handle < 0)
        {
            return nullptr;
        }
        uint32_t const raw = static_cast<uint32_t>(handle);
        size_t const idx = raw & 0xFFFFu;
        if (idx >= KICKOS_MAX_DOMAINS)
        {
            return nullptr;
        }
        Domain* d = &kernel().domains[idx];
        if (d->generation != static_cast<uint16_t>(raw >> 16))
        {
            return nullptr; // the slot has been reclaimed since this handle was minted
        }
        if (d->refcount == 0 and not d->immortal)
        {
            return nullptr; // free slot: a live handle to one cannot exist
        }
        return d;
    }

    unsigned domain_space_id(Domain const* d)
    {
#if KICKOS_HAVE_ASPACE
        if (d == nullptr or d->space == nullptr)
        {
            return 0;
        }
        // The slot index biased by one, so 0 stays "no space" and no kernel address
        // crosses the syscall boundary.
        return static_cast<unsigned>(d - &kernel().domains[0]) + 1u;
#else
        (void)d;
        return 0;
#endif
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
        if (d == nullptr or d->space == nullptr)
        {
            return nullptr;
        }
        return &d->ranges;
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
        (void)donor;
        if ((caller & DOM_CALLER_PRIVILEGED) != 0)
        {
            return domain_kernel();
        }
        if (mem_base == nullptr or mem_size == 0)
        {
#if KICKOS_HAVE_ASPACE
            return claim_slot(err);
#else
            return domain_default_user();
#endif
        }
        uintptr_t const base = reinterpret_cast<uintptr_t>(mem_base);
        // Access is the grant's own; only the memory-type bits come from the caller.
        uint32_t const attr = ARCH_MPU_R | ARCH_MPU_W | (mem_attr & ARCH_MPU_NOCACHE);
#if KICKOS_HAVE_ASPACE
        // Admission on this backend is the handoff below: the range must be one the donor
        // reserved, which no MMIO block and no kernel address can be.
        if (not grant_nocache_admissible(attr))
        {
            *err = KOS_EPERM; // a memory type this chip cannot honour
            return nullptr;
        }
#else
        size_t const rsz = arch_ram_region_size(mem_size);
        // Must run before a slot is allocated: a refusal leaves no half-built domain.
        if (not grant_region_admissible(base, rsz, attr,
                                        (caller & DOM_CALLER_MEM_AUTH) != 0))
        {
            *err = KOS_EPERM; // out-of-arena / reserved block / unhonourable memory type
            return nullptr;
        }
#endif
        Domain* d = claim_slot(err);
        if (d == nullptr)
        {
            return nullptr;
        }
#if KICKOS_HAVE_ASPACE
        // A refusal leaves no half-built domain: the release below frees the space and the slot.
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
        d->regions[0].size = rsz;
        d->regions[0].attr = attr;
        d->region_count = 1;
#endif
        return d;
    }

    // The refcount counts live tasks, creator holds, and CAP_ASPACE capabilities. Only the
    // first two are bounded by the task pool, which is what this assert covers; capability
    // holds are bounded at obj_ref_inc, which refuses at the ceiling.
    static_assert(2ull * KICKOS_MAX_TASKS <= UINT16_MAX,
                  "Domain::refcount is uint16_t and counts live tasks plus creator holds: "
                  "twice the task pool must fit it");

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
            // UINT16_MAX and not twice the task pool: a CAP_ASPACE capability is a holder
            // bounded by no pool of tasks.
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

// Reads the linker-defined app code (RX) and static data/.bss (RW-NX) sections into the regions
// every unprivileged thread needs under MPU enforcement. The symbols are optional
// (kickos/klink.h), so `end > start` is the test: a `start != 0` guard would read a valid base as
// absent on a flash-at-0 chip (K64F, __kickos_code_start == 0).
extern "C"
{
    extern unsigned char __kickos_code_start[] KICKOS_LINK_OPTIONAL;
    extern unsigned char __kickos_code_end[] KICKOS_LINK_OPTIONAL;
    extern unsigned char __kickos_appdata_start[] KICKOS_LINK_OPTIONAL;
    extern unsigned char __kickos_appdata_end[] KICKOS_LINK_OPTIONAL;

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
