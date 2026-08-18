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
        // every unprivileged thread with no explicit grant shares it.
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

    Domain* domain_for(bool privileged, void* mem_base, size_t mem_size,
                       bool caller_authorized, int* err)
    {
        *err = 0;
        if (privileged)
        {
            return domain_kernel();
        }
        if (mem_base == nullptr or mem_size == 0)
        {
            return domain_default_user();
        }
        uintptr_t const base = reinterpret_cast<uintptr_t>(mem_base);
        size_t const rsz = arch_ram_region_size(mem_size);
        // Rule 7 admits the PROSPECTIVE COMMITTED geometry, and must run before a slot is
        // allocated so a refusal is a clean failure, not a half-built domain.
        if (not grant_region_admissible(base, rsz, ARCH_MPU_R | ARCH_MPU_W,
                                        caller_authorized))
        {
            *err = KOS_EPERM; // out-of-arena / reserved-block hit: never admissible
            return nullptr;
        }
        Kernel& k = kernel();
        // Groups sharing one region share a domain, so a live unprivileged domain describing
        // exactly this region is reused. The match is on the ROUNDED size, so a re-grant of
        // the same block dedups. It is a slot economy and NOT an expression of intent: which
        // threads form a group is what a Task says, and two tasks landing on one domain here
        // stay two tasks (docs/design-task-layer.md open question 2).
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
        Domain* d = free_slot();
        if (d == nullptr)
        {
            *err = KOS_ENOMEM; // pool exhausted: retry later
            return nullptr;
        }
        *d = Domain{};
        d->privileged = false;
        d->regions[0].base = base;
        d->regions[0].size = rsz;
        d->regions[0].attr = ARCH_MPU_R | ARCH_MPU_W;
        d->region_count = 1;
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
