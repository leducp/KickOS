// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Memory domains, the unit of memory isolation: a shared region set (data/heap RW-NX plus
// any granted MMIO; code is covered by the privileged background map on hardware) and a
// privilege posture. On a translating backend a domain ALSO holds the address space its
// task's threads run under, one per task, and the region set is validation data beside it
// rather than the enforcement.
//
// Each thread additionally carries its own stack region, layered on at switch-in
// composition (thread.cc). Under an MPU that region is per-thread and a sibling faults on
// it; under translation the mapping is task-wide and a sibling reaches it. The portable
// floor is the weaker of the two: a thread-scoped grant guarantees access to its HOLDER,
// and portable code may not rely on a sibling being denied (docs/design-m6-mmu.md F9).
//
// Non-immortal domains are refcounted by the live TASKS holding them (task.h), plus one
// reference per EXPLICIT task's creator hold, and returned to the pool at zero.
//
// A granted DEV window is NOT here: it has exactly one holder and a domain is shared, so
// thread.cc composes it per-thread. docs/design-task-layer.md section 5.2.

#ifndef KICKOS_DOMAIN_H
#define KICKOS_DOMAIN_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h>
#include <kickos/config.h> // KICKOS_MPU_MAX_REGIONS

namespace kickos
{
    struct Domain
    {
        // READ THROUGH THE ACCESSORS BELOW ONLY, from anywhere outside domain.cc. The
        // shape here is the region backend's; the translating one answers the same
        // accessors while its enforcement lives in `space`.
        //
        // Shared regions (attr = unprivileged rights; supervisor comes from the
        // background region / SYSMPU RGD0). The thread's stack is NOT here: it is added
        // when the thread's region set is composed.
        arch_mpu_region regions[KICKOS_MPU_MAX_REGIONS] = {};
        size_t region_count = 0;
#if KICKOS_HAVE_ASPACE
        // The address space this domain's task runs under. Created with the domain and
        // destroyed at the last release; null on the two immortal singletons, neither of
        // which is ever a task's own space.
        struct arch_aspace* space = nullptr;
#endif
        bool privileged = false;
        bool immortal = false; // kernel + default-user singletons: never freed
        // Live tasks holding this domain, plus one per explicit task's creator hold;
        // 0 and not immortal => free slot.
        uint16_t refcount = 0;
    };

    // The ONLY sanctioned way to read a domain's regions from outside domain.cc. count is
    // null-safe and returns 0; domain_region_at requires i < domain_region_count(d).
    size_t domain_region_count(Domain const* d);
    arch_mpu_region const* domain_region_at(Domain const* d, size_t i);

    // A small stable name for the address space this domain holds, or 0 for none. NOT a
    // kernel pointer: the selftest compares two of these across tasks and must learn
    // nothing else from either. Null-safe.
    unsigned domain_space_id(Domain const* d);

    // Boot: build the two immortal domains (kernel = whole arena/privileged,
    // default-user = empty/unprivileged). Call once, after arch_init.
    void domain_init(void);

    Domain* domain_kernel(void); // privileged, whole arena
    // Unprivileged, empty region set. On a region backend it is the domain every no-grant
    // task JOINS; on a translating one it is a TEMPLATE each such task instantiates into a
    // slot of its own, since joining would put two kill groups in one address space
    // (docs/design-m6-mmu.md F2).
    Domain* domain_default_user(void);

    // Resolve the domain a group of threads shares. privileged -> kernel; with a data
    // region -> a domain of its own; otherwise -> the default-user domain or an instance
    // of it. Does NOT take a reference (task_ref does, from thread_create).
    //
    // NO REUSE ACROSS TASKS. Two tasks granting the same block get two domains, because a
    // domain is the address space a task's threads share and collapsing two tasks into one
    // makes "the set of threads sharing a domain" name something bigger than a task (F2).
    // What the old reuse arm keyed on was a PHYSICAL base, which only means anything while
    // the map is 1:1.
    //
    // mem_attr is the MEMORY TYPE the region is committed with, over the R|W every RAM grant
    // already has: ARCH_MPU_NOCACHE and nothing else today.
    //
    // The Rule 7 chokepoint for a SHARED grant: the PROSPECTIVE COMMITTED geometry (rounded
    // to arch_ram_region_size, R|W plus mem_attr) goes through grant_region_admissible
    // before a slot is taken. caller_authorized is the GRANTING thread's AUTH_MEMORY answer
    // and MUST come from the caller; it is never read from sched::current() here. A
    // per-THREAD grant is admitted at the spawn boundary instead.
    //
    // Returns null on refusal and writes the reason to *err (never null; 0 on success):
    //   KOS_EPERM   inadmissible grant: reserved-block hit, out-of-arena, or a memory type
    //               this chip cannot honour
    //   KOS_ENOMEM  the domain pool is full, or no address space could be built
    Domain* domain_for(bool privileged, void* mem_base, size_t mem_size, uint32_t mem_attr,
                       bool caller_authorized, int* err);

    // Held by the TASK, not by each of its threads: task_ref, task_release and the explicit
    // task's creator hold (task.h) are the only callers outside domain_init.
    void domain_ref(Domain* d);     // a task joins the domain
    void domain_release(Domain* d); // a task leaves; frees the slot at zero
}

#endif
