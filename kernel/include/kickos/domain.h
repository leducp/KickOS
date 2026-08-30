// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Memory domains, the unit of memory isolation: a shared region set and a privilege posture.
// On a translating backend a domain also holds the address space its task's threads run under,
// one per task.
//
// Each thread carries its own stack region, layered on at switch-in composition (thread.cc).
// Under an MPU that region is per-thread and a sibling faults on it; under translation the
// mapping is task-wide and a sibling reaches it. Portable code may rely only on the weaker
// floor: a thread-scoped grant guarantees access to its holder (docs/design-m6-mmu.md F9).
//
// Non-immortal domains are refcounted by the live tasks holding them, plus one reference per
// explicit task's creator hold, and returned to the pool at zero.

#ifndef KICKOS_DOMAIN_H
#define KICKOS_DOMAIN_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h>
#include <kickos/config.h> // KICKOS_MPU_MAX_REGIONS
#include <kickos/vrange.h>

namespace kickos
{
    // How many region descriptors one domain records. Where a backend translates only the
    // kernel singleton describes one, every grant going into the range list instead.
#if KICKOS_HAVE_ASPACE
    enum : size_t { KICKOS_DOMAIN_REGIONS = 1 };
#else
    enum : size_t { KICKOS_DOMAIN_REGIONS = KICKOS_MPU_MAX_REGIONS };
#endif
    static_assert(KICKOS_DOMAIN_REGIONS <= KICKOS_MPU_MAX_REGIONS,
                  "a domain may not record more regions than an MPU image can hold");

    struct Domain
    {
        // Read through the accessors below only, from anywhere outside domain.cc. Shared
        // regions; attr is unprivileged rights, supervisor coming from the background region.
        arch_mpu_region regions[KICKOS_DOMAIN_REGIONS] = {};
#if KICKOS_HAVE_ASPACE
        // The address space this domain's task runs under. Created with the domain and
        // destroyed at the last release; null on the two immortal singletons.
        struct arch_aspace* space = nullptr;
        // The domain whose reservation this one borrows, or null. Holds one reference on that
        // domain, taken when the handoff succeeds and dropped in drop_space. Written once, at
        // the borrower's construction, so the edges form a forest and no cycle can arise.
        Domain* borrowed_from = nullptr;
        // What that space names, and the syscall entry's oracle. Seeded with the space and
        // thrown away with it.
        VirtualRanges ranges;
#endif
        // Bumped on every claim, so a capability naming (index, generation) cannot be answered
        // by a later occupant. NOT reset by claim_slot's reinitialisation, which would defeat it.
        uint16_t generation = 0;
        // Live tasks holding this domain, plus one per explicit task's creator hold;
        // 0 and not immortal => free slot.
        uint16_t refcount = 0;
        uint8_t region_count = 0;
        bool privileged = false;
        bool immortal = false; // kernel + default-user singletons: never freed
    };
    static_assert(KICKOS_DOMAIN_REGIONS <= UINT8_MAX,
                  "Domain::region_count is uint8_t and counts this domain's descriptors");

    // The only sanctioned way to read a domain's regions from outside domain.cc. count is
    // null-safe and returns 0; domain_region_at requires i < domain_region_count(d).
    size_t domain_region_count(Domain const* d);
    arch_mpu_region const* domain_region_at(Domain const* d, size_t i);

    // A small stable name for the address space this domain holds, or 0 for none. Not a kernel
    // pointer: the selftest compares two of these across tasks and must learn nothing else.
    // Null-safe.
    unsigned domain_space_id(Domain const* d);

    // The (index, generation) handle a CAP_ASPACE entry stores, and its inverse; SlotPool's
    // codec. domain_resolve answers null for a slot whose generation has moved.
    int domain_handle(Domain const* d);
    Domain* domain_resolve(int handle);

    // The live hold count, for the ONE site that refuses at the ceiling. Null-safe.
    uint16_t domain_refcount(Domain const* d);

    // The address space this domain's task runs under, or null where the backend
    // translates nothing and on the two immortal singletons. Null-safe.
    struct arch_aspace* domain_space(Domain const* d);

#if KICKOS_HAVE_ASPACE
    // The granted-range list of that space, or null on a domain holding none. Null-safe.
    VirtualRanges const* domain_ranges(Domain const* d);

    // The same list, writable. Null-safe.
    VirtualRanges* domain_ranges_mut(Domain* d);

    // How many pool slots hold an address space at all, live or free. A free slot legitimately
    // holds one, so a count above the number of live tasks is not a leak by itself.
    size_t domain_spaces_held(void);
#endif

    // Boot: build the two immortal domains (kernel = whole arena/privileged,
    // default-user = empty/unprivileged). Call once, after arch_init.
    void domain_init(void);

    Domain* domain_kernel(void); // privileged, whole arena
    // Unprivileged, empty region set. On a region backend every no-grant task joins it; on a
    // translating one each such task instantiates it into a slot of its own (F2).
    Domain* domain_default_user(void);

    // The caller's posture as one word: domain_for and task_for sit on the deepest chain the
    // syscall dispatch has, and every further argument is a word the trap red zone of a
    // KICKOS_KERNEL_STACKS=0 board is measured against.
    enum : uint32_t
    {
        DOM_CALLER_PRIVILEGED = 1u << 0, // resolves the kernel domain, whole arena
        DOM_CALLER_MEM_AUTH = 1u << 1    // the GRANTING thread's AUTH_MEMORY answer
    };

    // Resolve the domain a group of threads shares. privileged -> kernel; with a data region ->
    // a domain of its own; otherwise -> the default-user domain or an instance of it. Does not
    // take a reference (task_ref does, from thread_create). No reuse across tasks: two tasks
    // granting the same block get two domains (F2).
    //
    // mem_attr is the memory type the region is committed with, over the R|W every RAM grant
    // already has. The prospective committed geometry goes through grant_region_admissible before
    // a slot is taken; DOM_CALLER_MEM_AUTH is the granting thread's AUTH_MEMORY answer and must
    // come from the caller, never read from sched::current() here.
    //
    // `donor` is the domain the grant's memory was reserved in; where a backend translates the
    // block is mapped into the new space at the same virtual address and recorded borrowed. A
    // successful handoff takes a reference on the donor, which is why this is not const.
    //
    // Returns null on refusal and writes the reason to *err (never null; 0 on success):
    //   KOS_EPERM   inadmissible grant: reserved-block hit, out-of-arena, a memory type
    //               this chip cannot honour, or a range the donor never reserved
    //   KOS_ENOMEM  the domain pool is full, no address space could be built, or the new
    //               space cannot take the range at the donor's address
    Domain* domain_for(uint32_t caller, void* mem_base, size_t mem_size, uint32_t mem_attr,
                       Domain* donor, int* err);

    // Held by the task, not by each of its threads. Three kinds of holder call these outside
    // domain_init: task_ref/task_release, the explicit task's creator hold, and the handoff's
    // hold on the donor, which drop_space hands back for its caller to release.
    void domain_ref(Domain* d);     // a task joins the domain
    void domain_release(Domain* d); // a task leaves; frees the slot at zero
}

#endif
