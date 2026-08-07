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
        // background region / SYSMPU RGD0). The per-thread stack is NOT here: it is
        // private and added when the thread's region set is composed.
        arch_mpu_region regions[KICKOS_MPU_MAX_REGIONS] = {};
        size_t region_count = 0;
        bool privileged = false;
        bool immortal = false; // kernel + default-user singletons: never freed
        uint16_t refcount = 0; // live threads; 0 and not immortal => free slot
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

    // Resolve the domain a thread belongs to. privileged -> kernel; unprivileged
    // with a data region -> find-or-create shared by (base,size); otherwise ->
    // default-user. An MMIO grant (mmio_base != 0) is a capability: it ALWAYS gets a
    // fresh, unshared domain carrying {data region?, MMIO region R|W|DEV}. Does NOT
    // take a reference (thread_create does, via domain_ref).
    //
    // The Rule 7 chokepoint: the PROSPECTIVE COMMITTED geometry (data region rounded to
    // arch_ram_region_size, R|W; MMIO exact, R|W|DEV) goes through
    // grant_region_admissible at entry, after the privileged and no-grant short-circuits
    // and before the dedup. caller_authorized is the SPAWNER's AUTH_MEMORY answer and MUST
    // be resolved by the caller; it is never read from sched::current() here.
    //
    // An MMIO grant is additionally EXCLUSIVE: a DEV window overlapping one a LIVE domain
    // already holds is refused -KOS_EBUSY, with no stealing. Matched on range overlap,
    // where adjacency is not overlap, so two flush-but-disjoint windows both admit. The
    // check and the commit (domain_ref, in thread_create) both sit inside thread_spawn's
    // function-scope IrqLock, so they are atomic together.
    //
    // A respawn issued while the dying holder still references its domain earns
    // -KOS_EBUSY. sched::exit_current drops the reference BEFORE the cap_teardown sweep
    // that EPIPE-wakes a respawner, so a woken supervisor always observes the window
    // already free. A supervisor that learns of the death some OTHER way (a watchdog, a
    // timeout) must retry on -KOS_EBUSY, or wait the death out with kos_thread_join before
    // respawning.
    //
    // Returns null on refusal and writes the reason to *err (never null; 0 on success):
    //   KOS_EPERM   the grant is inadmissible (reserved-block hit, out-of-arena data,
    //               unauthorized DEV). Fix the grant.
    //   KOS_EINVAL  malformed geometry (an MMIO base with a zero extent).
    //   KOS_EBUSY   a live domain already holds an overlapping DEV window.
    //   KOS_ENOMEM  the domain pool is full. Retry later.
    // This is the authoritative admission: the spawn boundary forwards *err rather than
    // re-checking the same predicates.
    Domain* domain_for(bool privileged, void* mem_base, size_t mem_size,
                       void* mmio_base, size_t mmio_size, bool caller_authorized,
                       int* err);

    void domain_ref(Domain* d);     // a thread joins the domain
    void domain_release(Domain* d); // a thread leaves; frees the slot at zero

    // True iff NO live domain holds a DEV region overlapping [base, base+size). The
    // admission test behind the one-holder-per-window rule above, and ALSO the console
    // reclaim's precondition: re-initialising a device whose window a live thread still
    // holds corrupts it under that thread. Callers pass a non-wrapping window.
    bool dev_window_free(uintptr_t base, size_t size);
}

#endif
