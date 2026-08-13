// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Memory domains, the unit of memory isolation. A domain owns a shared region
// set (data/heap RW-NX plus any granted MMIO; code is covered by the privileged
// background map on hardware) and a privilege posture. Threads belong to a
// domain and share its memory, but each thread additionally carries its OWN
// private stack region, layered on at switch-in composition (thread.cc), so a
// sibling cannot scribble another's stack.
//
// "Threads sharing one region share a domain": an unprivileged thread spawned
// with a data region joins (find-or-create by base+size) the domain describing
// exactly that region. Privileged threads share the immortal kernel domain
// (the whole arena); unprivileged threads with no granted region share the
// immortal default-user domain (an empty region set). Non-immortal domains are
// refcounted by the live TASKS holding them (task.h), plus one reference per
// EXPLICIT task's creator hold, and returned to the pool at zero.
//
// A granted DEV window is NOT here: it belongs to the one thread that asked for it
// (thread.cc composes it), because a domain is shared and a window has exactly one
// holder. docs/design-task-layer.md section 5.2.

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
        // Shared regions (attr = unprivileged rights; supervisor comes from the
        // background region / SYSMPU RGD0). The per-thread stack is NOT here: it is
        // private and added when the thread's region set is composed.
        arch_mpu_region regions[KICKOS_MPU_MAX_REGIONS] = {};
        size_t region_count = 0;
        bool privileged = false;
        bool immortal = false; // kernel + default-user singletons: never freed
        // Live tasks holding this domain, plus one per explicit task's creator hold;
        // 0 and not immortal => free slot.
        uint16_t refcount = 0;
    };

    // The ONLY sanctioned way to read a domain's regions from outside domain.cc, which
    // owns the backing store. count is null-safe and returns 0; domain_region_at requires
    // i < domain_region_count(d).
    size_t domain_region_count(Domain const* d);
    arch_mpu_region const* domain_region_at(Domain const* d, size_t i);

    // Boot: build the two immortal domains (kernel = whole arena/privileged,
    // default-user = empty/unprivileged). Call once, after arch_init.
    void domain_init(void);

    Domain* domain_kernel(void);       // privileged, whole arena
    Domain* domain_default_user(void); // unprivileged, empty region set

    // Resolve the domain a group of threads shares. privileged -> kernel; with a data
    // region -> find-or-create shared by (base,size); otherwise -> default-user. Does NOT
    // take a reference (task_ref does, from thread_create).
    //
    // The Rule 7 chokepoint for a SHARED grant: the PROSPECTIVE COMMITTED geometry (rounded
    // to arch_ram_region_size, R|W) goes through grant_region_admissible at entry, after the
    // privileged and no-grant short-circuits and before the dedup. caller_authorized is the
    // GRANTING thread's AUTH_MEMORY answer and MUST be resolved by the caller; it is never
    // read from sched::current() here. A per-THREAD grant is admitted at the spawn boundary
    // instead, which is where it is asked for.
    //
    // Returns null on refusal and writes the reason to *err (never null; 0 on success):
    //   KOS_EPERM   the grant is inadmissible (reserved-block hit, out-of-arena). Fix it.
    //   KOS_ENOMEM  the domain pool is full. Retry later.
    Domain* domain_for(bool privileged, void* mem_base, size_t mem_size,
                       bool caller_authorized, int* err);

    // Held by the TASK, not by each of its threads: task_ref, task_release and the explicit
    // task's creator hold (task.h) are the only callers outside domain_init.
    void domain_ref(Domain* d);     // a task joins the domain
    void domain_release(Domain* d); // a task leaves; frees the slot at zero
}

#endif
