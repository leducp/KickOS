// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Memory domains -- the unit of memory isolation (M2). A domain owns a shared
// region set (data/heap RW-NX + any granted MMIO; code is covered by the
// privileged background map on hardware) plus a privilege posture. Threads
// belong to a domain and share its memory; each thread additionally carries its
// OWN private stack region, layered on at switch-in composition (thread.cc), so
// a sibling cannot scribble another's stack. Model: Zephyr k_mem_domain /
// ARINC 653 spatial partitions.
//
// "Threads sharing one region share a domain": an unprivileged thread spawned
// with a data region joins (find-or-create by base+size) the domain describing
// exactly that region. Privileged threads share the immortal kernel domain
// (the whole arena); unprivileged threads with no granted region share the
// immortal default-user domain (an empty region set). Non-immortal domains are
// refcounted by their live threads and returned to the pool at the last exit.

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
        // background region / SYSMPU RGD0). NOT the per-thread stack -- that is
        // private and added when the thread's region set is composed.
        arch_mpu_region regions[KICKOS_MPU_MAX_REGIONS];
        size_t region_count;
        bool privileged;
        bool immortal;     // kernel + default-user singletons: never freed
        uint16_t refcount; // live threads; 0 and not immortal => free slot
    };

    // Boot: build the two immortal domains (kernel = whole arena/privileged,
    // default-user = empty/unprivileged). Call once, after arch_init.
    void domain_init(void);

    Domain* domain_kernel(void);       // privileged, whole arena
    Domain* domain_default_user(void); // unprivileged, empty region set

    // Resolve the domain a thread belongs to. privileged -> kernel; unprivileged
    // with a data region -> find-or-create shared by (base,size); otherwise ->
    // default-user. An MMIO grant (mmio_base != 0) is a capability: it ALWAYS gets a
    // fresh, unshared domain carrying {data region?, MMIO region R|W|DEV}. Does NOT
    // take a reference (thread_create does, via domain_ref). Returns null ONLY when a
    // new slot is needed but the pool is exhausted.
    Domain* domain_for(bool privileged, void* mem_base, size_t mem_size,
                       void* mmio_base, size_t mmio_size);

    void domain_ref(Domain* d);     // a thread joins the domain
    void domain_release(Domain* d); // a thread leaves; frees the slot at zero
}

#endif
