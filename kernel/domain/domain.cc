// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/domain.h>

#include <kickos/aspace.h> // aspace_image_seed / aspace_release
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

        // A fresh unprivileged domain, with the address space it needs where a backend
        // translates. TRANSACTIONAL: on either refusal the slot is left reinitialised, so
        // it is free again and no half-built domain survives. *err is written on refusal.
        Domain* claim_slot(int* err)
        {
            Domain* d = free_slot();
            if (d == nullptr)
            {
                *err = KOS_ENOMEM; // pool exhausted: retry later
                return nullptr;
            }
#if KICKOS_HAVE_ASPACE
            // A FREE SLOT CAN STILL HOLD A SPACE. task_for resolves a domain before the
            // spawn commits and takes no reference, so a spawn that fails after that point
            // leaves the slot at refcount 0 with its root and tables still allocated, and
            // the reinitialisation below would drop the only handle to them.
            if (d->space != nullptr)
            {
                aspace_release(d->space, &d->ranges);
                d->space = nullptr;
            }
#endif
            *d = Domain{};
            d->privileged = false;
#if KICKOS_HAVE_ASPACE
            d->space = arch_aspace_create();
            if (d->space == nullptr)
            {
                *err = KOS_ENOMEM; // no frame for a root: freeing frames and retrying can work
                return nullptr;
            }
            // A space with no image is a space no unprivileged thread can run in: its first
            // instruction fetch would fault. So the seed is part of claiming the slot, and
            // its failure leaves the slot free rather than half built.
            if (not aspace_image_seed(d->space, &d->ranges))
            {
                aspace_release(d->space, &d->ranges);
                d->space = nullptr;
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
        // Kernel domain: the whole user-RAM arena, privileged. Immortal because
        // root, idle and every privileged thread reference it.
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
        // Default user domain: no granted arena region, unprivileged. Immortal because
        // it is what every unprivileged thread with no explicit grant resolves to: the
        // domain itself on a region backend, and the template it is copied from where a
        // domain carries an address space.
        Domain* const udom = &k.domains[KDOM_DEFAULT_USER_INDEX];
        udom->privileged = false;
        udom->immortal = true;
        udom->region_count = 0;
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
        return &kernel().domains[KDOM_KERNEL_INDEX];
    }

    Domain* domain_default_user(void)
    {
        return &kernel().domains[KDOM_DEFAULT_USER_INDEX];
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
                       Domain const* donor, int* err)
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
            // The template is instantiated, never joined: two no-grant tasks sharing one
            // root would be two kill groups in one address space, and that is the COMMON
            // case rather than an edge (docs/design-m6-mmu.md F2).
            return claim_slot(err);
#else
            return domain_default_user();
#endif
        }
        uintptr_t const base = reinterpret_cast<uintptr_t>(mem_base);
        // Access is the grant's own; only the memory-type bits come from the caller.
        uint32_t const attr = ARCH_MPU_R | ARCH_MPU_W | (mem_attr & ARCH_MPU_NOCACHE);
#if KICKOS_HAVE_ASPACE
        // NOT the arena predicate here. What a reservation names on this backend is frames
        // of the frame pool's own carve, which the bump arena does not contain, so the arena
        // arm would refuse every correct grant. The admission that replaces it is the handoff
        // below: the range must be one the DONOR reserved, which no MMIO block and no
        // kernel address can be.
        if (not grant_nocache_admissible(attr))
        {
            *err = KOS_EPERM; // a memory type this chip cannot honour
            return nullptr;
        }
#else
        size_t const rsz = arch_ram_region_size(mem_size);
        // Rule 7 admits the PROSPECTIVE COMMITTED geometry, and must run before a slot is
        // allocated: a refusal must leave no half-built domain.
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
        // F10's handoff, and the ONE place both of its consumers meet: a task create and a
        // grant-carrying spawn each open a space the donor does not hold, and each arrives
        // here. A refusal leaves no half-built domain, the release below freeing the space
        // and leaving the slot free.
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
        // NO REGION RECORD BESIDE THE MAPPING. The range list carries the handoff with the
        // EXACT extent where a region would carry the rounded one, and it is what the entry
        // path answers from on this backend (section 3.3).
#else
        d->regions[0].base = base;
        d->regions[0].size = rsz;
        d->regions[0].attr = attr;
        d->region_count = 1;
#endif
        return d;
    }

    // A mortal domain's refcount counts the live TASKS holding it, plus one per explicit
    // task's creator hold: a task takes one reference when its first thread joins (task_ref)
    // and drops it when its last leaves (task_release, from sched::exit_current), and
    // task_create takes one more that task_drop_hold returns. Both are bounded by the task
    // pool, so two per slot is the ceiling.
    static_assert(2ull * KICKOS_MAX_TASKS <= UINT16_MAX,
                  "Domain::refcount is uint16_t and counts live tasks plus creator holds: "
                  "twice the task pool must fit it");

    void domain_ref(Domain* d)
    {
        // An immortal domain's refcount is deliberately NOT tracked: an unbounded,
        // transient set of tasks references it and the counter would wrap.
        if (d != nullptr and not d->immortal)
        {
            // Trips on a reference held by something other than a live task or a creator.
            KICKOS_DEBUG_ASSERT(d->refcount < 2u * KICKOS_MAX_TASKS);
            d->refcount++;
        }
    }

    void domain_release(Domain* d)
    {
        // A mortal domain returns to the pool when its last task drops it. Immortal ones
        // never free and never count.
        if (d == nullptr or d->immortal)
        {
            return;
        }
        if (d->refcount > 0)
        {
            d->refcount--;
        }
#if KICKOS_HAVE_ASPACE
        // The slot alone is not the resource here: reusing it without this would strand the
        // root, its tables and every frame the space still held.
        if (d->refcount == 0 and d->space != nullptr)
        {
            aspace_release(d->space, &d->ranges);
            d->space = nullptr;
        }
#endif
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
