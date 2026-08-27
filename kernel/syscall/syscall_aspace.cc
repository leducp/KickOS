// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test scaffolding for the address-space seam (KOS_SYS_ASPACE_PROBE). The map editor is a
// kernel seam and never a syscall, so each op runs a whole scenario here and answers a
// number; nothing hands userspace a mapping primitive.
//
// The addresses are deliberately not the frame's: an identity map answers every arm of
// this step correctly, so a scenario that mapped a frame at its own output address would
// prove only that the memory exists (docs/design-m6-mmu.md section 3.2).

#include <kickos/arch/arch.h>

#include "syscall_internal.h"

#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)

#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/reent.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

namespace kickos
{
    namespace
    {
        // Two low-half pages, far from DRAM's output addresses and from each other, so
        // neither could be answered by an identity map of the frame under test.
        constexpr uintptr_t VA_A = 0x10000000;
        constexpr uintptr_t VA_B = 0x11000000;
        constexpr uint32_t PATTERN_A = 0xA5A50F0Fu;
        constexpr uint32_t PATTERN_B = 0x5A5AF0F0u;

        uint32_t volatile* word_at(void* p)
        {
            return static_cast<uint32_t volatile*>(p);
        }

        // Reached through the acquire pair rather than through the running translation, so
        // an arm that fails leaves the caller alive to report it.
        uint64_t op_roundtrip()
        {
            struct arch_aspace* const space = arch_aspace_create();
            if (space == nullptr)
            {
                return 0;
            }
            uint64_t trip = 0;
            bool mapped = false;
            arch_phys_addr_t const frame = kickos_frame_alloc();
            if (frame != 0 and
                arch_aspace_map(space, VA_A, frame, 1, ARCH_MAP_R | ARCH_MAP_W,
                                ARCH_MAP_NORMAL) == ARCH_ASPACE_OK)
            {
                trip = KOS_ASPACE_TRIP_MAPPED;
                mapped = true;
                void* const p = arch_aspace_acquire(space, VA_A);
                if (p != nullptr)
                {
                    *word_at(p) = PATTERN_A;
                    if (*word_at(p) == PATTERN_A)
                    {
                        trip = KOS_ASPACE_TRIP_READBACK;
                    }
                    arch_aspace_release(space, VA_A);
                }
                if (trip == KOS_ASPACE_TRIP_READBACK and
                    arch_aspace_unmap(space, VA_A, 1) == ARCH_ASPACE_OK)
                {
                    trip = KOS_ASPACE_TRIP_UNMAPPED;
                    mapped = false;
                    if (arch_aspace_acquire(space, VA_A) == nullptr)
                    {
                        trip = KOS_ASPACE_TRIP_GONE;
                    }
                }
            }
            arch_aspace_destroy(space);
            if (frame != 0 and not mapped)
            {
                // Destroy reclaims what a space still holds, so a frame no leaf points at
                // when it runs is the caller's to return.
                kickos_frame_free(frame);
            }
            return trip;
        }

        // An identity map cannot produce this: two unequal virtual pages onto one frame
        // would give two different pages.
        uint64_t op_alias()
        {
            struct arch_aspace* const space = arch_aspace_create();
            if (space == nullptr)
            {
                return 0;
            }
            uint64_t ok = 0;
            arch_phys_addr_t const frame = kickos_frame_alloc();
            void* const own = frame_pool_ptr(frame);
            if (frame != 0 and own != nullptr and
                arch_aspace_map(space, VA_A, frame, 1, ARCH_MAP_R | ARCH_MAP_W,
                                ARCH_MAP_NORMAL) == ARCH_ASPACE_OK and
                arch_aspace_map(space, VA_B, frame, 1, ARCH_MAP_R | ARCH_MAP_W,
                                ARCH_MAP_NORMAL) == ARCH_ASPACE_OK)
            {
                void* const a = arch_aspace_acquire(space, VA_A);
                void* const b = arch_aspace_acquire(space, VA_B);
                // The pool reaches the frame by its own route, so agreement here is two
                // independent answers rather than one restated.
                bool const same_frame = a == own and b == own;
                bool const translated = VA_A != VA_B and
                                        reinterpret_cast<uintptr_t>(a) != VA_A and
                                        static_cast<uintptr_t>(frame) != VA_A;
                if (a != nullptr and b != nullptr and same_frame and translated)
                {
                    *word_at(a) = PATTERN_A;
                    if (*word_at(b) == PATTERN_A and *word_at(own) == PATTERN_A)
                    {
                        ok = 1;
                    }
                }
                arch_aspace_release(space, VA_A);
                arch_aspace_release(space, VA_B);
                // One of the two leaves goes before destroy walks the tree: both name the
                // same frame, and destroy frees what a space maps.
                (void)arch_aspace_unmap(space, VA_B, 1);
            }
            arch_aspace_destroy(space);
            return ok;
        }

        uint64_t op_refusals()
        {
            struct arch_aspace* const space = arch_aspace_create();
            if (space == nullptr)
            {
                return 0;
            }
            size_t const g = arch_aspace_granule();
            arch_phys_addr_t const frame = kickos_frame_alloc();
            if (frame == 0)
            {
                arch_aspace_destroy(space);
                return 0;
            }
            uint32_t const rw = ARCH_MAP_R | ARCH_MAP_W;
            uint64_t bits = 0;
            // The kernel's own arena, granule-aligned: a high-half address by construction
            // on any port whose kernel lives there, rather than a literal this file invents.
            uintptr_t const high = arch_ram_base() & ~static_cast<uintptr_t>(g - 1);
            if (arch_aspace_map(space, high, frame, 1, rw, ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
            {
                bits |= KOS_ASPACE_REFUSE_HIGH_HALF;
            }
            if (arch_aspace_map(space, VA_A + 1, frame, 1, rw, ARCH_MAP_NORMAL) !=
                ARCH_ASPACE_OK)
            {
                bits |= KOS_ASPACE_REFUSE_UNALIGNED;
            }
            if (arch_aspace_map(space, VA_A, frame, 0, rw, ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
            {
                bits |= KOS_ASPACE_REFUSE_EMPTY;
            }
            if (arch_aspace_map(space, VA_A, frame, 1, ARCH_MAP_W, ARCH_MAP_NORMAL) !=
                ARCH_ASPACE_OK)
            {
                bits |= KOS_ASPACE_REFUSE_NO_READ;
            }
            if (arch_aspace_map(space, VA_A, frame, 1, rw | 0x80u, ARCH_MAP_NORMAL) !=
                ARCH_ASPACE_OK)
            {
                bits |= KOS_ASPACE_REFUSE_UNKNOWN_RIGHT;
            }
            if (arch_aspace_map(space, VA_A, frame, 1, rw | ARCH_MAP_X, ARCH_MAP_NORMAL) !=
                ARCH_ASPACE_OK)
            {
                bits |= KOS_ASPACE_REFUSE_WRITE_EXEC;
            }
            // One page mapped of a two-page unmap: the range is not wholly mapped, so the
            // call must refuse and clear nothing.
            if (arch_aspace_map(space, VA_A, frame, 1, rw, ARCH_MAP_NORMAL) == ARCH_ASPACE_OK)
            {
                if (arch_aspace_unmap(space, VA_A, 2) != ARCH_ASPACE_OK and
                    arch_aspace_acquire(space, VA_A) != nullptr)
                {
                    bits |= KOS_ASPACE_REFUSE_PART_UNMAP;
                }
            }
            arch_aspace_destroy(space);
            return bits;
        }

        // Every frame a whole cycle took must come back, tables included.
        uint64_t op_balance()
        {
            size_t const before = frame_pool_free();
            for (int i = 0; i < 4; i++)
            {
                struct arch_aspace* const space = arch_aspace_create();
                if (space == nullptr)
                {
                    return before;
                }
                arch_phys_addr_t const frame = kickos_frame_alloc();
                if (frame == 0)
                {
                    arch_aspace_destroy(space);
                    return before;
                }
                (void)arch_aspace_map(space, VA_A, frame, 1, ARCH_MAP_R | ARCH_MAP_W,
                                      ARCH_MAP_NORMAL);
                (void)arch_aspace_map(space, VA_B, frame, 1, ARCH_MAP_R,
                                      ARCH_MAP_NORMAL);
                (void)arch_aspace_unmap(space, VA_B, 1);
                // The leaf still mapped at VA_A is the space's to reclaim: destroy, not
                // unmap, owns a frame's life.
                arch_aspace_destroy(space);
            }
            // Every op, not just this cycle: no scenario in this file may hand the pool a
            // frame it does not own, and the allocator absorbs both a double free and a
            // stranger's address, so this counter is the only place either surfaces.
            if (frame_pool_refused() != 0)
            {
                return ~static_cast<uint64_t>(0);
            }
            size_t const after = frame_pool_free();
            if (after >= before)
            {
                return 0;
            }
            return before - after;
        }

        // A range that crosses two level-3 table boundaries, which is what a process image
        // is and what the index arithmetic gets wrong if a table's last slot is miscounted.
        // The output addresses are device space, so no leaf here names a pool frame and the
        // destroy below cannot reclaim one it does not own.
        uint64_t op_span()
        {
            constexpr uintptr_t SPAN_VA = 0x201FF000;   // the last slot of its level-3 table
            constexpr arch_phys_addr_t SPAN_PA = 0x08000000;
            constexpr size_t SPAN_PAGES = 600;          // one, then a whole table, then 87
            struct arch_aspace* const space = arch_aspace_create();
            if (space == nullptr)
            {
                return 0;
            }
            size_t const g = arch_aspace_granule();
            uint64_t ok = 0;
            if (arch_aspace_map(space, SPAN_VA, SPAN_PA, SPAN_PAGES, ARCH_MAP_R,
                                ARCH_MAP_DEVICE) == ARCH_ASPACE_OK)
            {
                // One page held at a time beside the reference, so this walk stays inside
                // ARCH_ASPACE_ACQUIRE_MIN: a backend with a finite window pool would
                // otherwise run out here for a reason that is this loop's and not the seam's.
                unsigned char* const first =
                    static_cast<unsigned char*>(arch_aspace_acquire(space, SPAN_VA));
                bool contiguous = first != nullptr;
                for (size_t i = 1; i < SPAN_PAGES and contiguous; i++)
                {
                    unsigned char* const at = static_cast<unsigned char*>(
                        arch_aspace_acquire(space, SPAN_VA + i * g));
                    contiguous = at == first + i * g;
                    arch_aspace_release(space, SPAN_VA + i * g);
                }
                arch_aspace_release(space, SPAN_VA);
                bool const bounded =
                    arch_aspace_acquire(space, SPAN_VA + SPAN_PAGES * g) == nullptr and
                    arch_aspace_acquire(space, SPAN_VA - g) == nullptr;
                bool const removed =
                    arch_aspace_unmap(space, SPAN_VA, SPAN_PAGES) == ARCH_ASPACE_OK and
                    arch_aspace_acquire(space, SPAN_VA) == nullptr and
                    arch_aspace_acquire(space, SPAN_VA + (SPAN_PAGES - 1) * g) == nullptr;
                if (contiguous and bounded and removed)
                {
                    ok = 1;
                }
            }
            arch_aspace_destroy(space);
            return ok;
        }

        // --- The page split, witnessed where no caller can build it ------------------
        // A validated range contiguous in virtual memory need not be contiguous in
        // physical memory, and no userspace arm can construct one here: a reservation's
        // virtual address is its output address, so virtual adjacency and physical
        // adjacency are the same question from a caller (section 3.4). The scenario is
        // therefore built with the map editor: three consecutive frames, the outer two
        // mapped at two adjacent virtual pages and the middle one left unmapped.
        //
        // The middle frame is the instrument: it is where a copy written as one memcpy
        // over a translated base spills, so reading it back separates a helper that
        // splits from one that merely appears to work on the first page.
        uint64_t op_split_access()
        {
            constexpr size_t HALF = 8; // bytes each side of the page boundary
            size_t const g = arch_aspace_granule();
            size_t const before = frame_pool_free();
            uint32_t const rw = ARCH_MAP_R | ARCH_MAP_W;
            struct arch_aspace* const sa = arch_aspace_create();
            struct arch_aspace* const sb = arch_aspace_create();
            arch_phys_addr_t const ra = frame_pool_alloc_run(3);
            arch_phys_addr_t const rb = frame_pool_alloc_run(3);
            uint64_t bits = 0;
            if (sa != nullptr and sb != nullptr and ra != 0 and rb != 0)
            {
                arch_phys_addr_t const step = static_cast<arch_phys_addr_t>(g);
                // The same two virtual pages in both spaces, which is what makes the copy
                // below a pair of equal numbers naming different memory.
                bool const built =
                    arch_aspace_map(sa, VA_A, ra, 1, rw, ARCH_MAP_NORMAL) == ARCH_ASPACE_OK and
                    arch_aspace_map(sa, VA_A + g, ra + 2 * step, 1, rw, ARCH_MAP_NORMAL) ==
                        ARCH_ASPACE_OK and
                    arch_aspace_map(sb, VA_A, rb, 1, rw, ARCH_MAP_NORMAL) == ARCH_ASPACE_OK and
                    arch_aspace_map(sb, VA_A + g, rb + 2 * step, 1, rw, ARCH_MAP_NORMAL) ==
                        ARCH_ASPACE_OK;
                unsigned char* const alo =
                    static_cast<unsigned char*>(arch_aspace_acquire(sa, VA_A));
                unsigned char* const ahi =
                    static_cast<unsigned char*>(arch_aspace_acquire(sa, VA_A + g));
                unsigned char* const blo =
                    static_cast<unsigned char*>(arch_aspace_acquire(sb, VA_A));
                unsigned char* const bhi =
                    static_cast<unsigned char*>(arch_aspace_acquire(sb, VA_A + g));
                // Reached by the pool's own route, so it is never mapped in either space and
                // an access that lands there did not split.
                unsigned char* const spill =
                    static_cast<unsigned char*>(frame_pool_ptr(ra + step));
                if (built and alo != nullptr and ahi != nullptr and blo != nullptr and
                    bhi != nullptr and spill != nullptr)
                {
                    uintptr_t const cross = VA_A + g - HALF;
                    if (ahi != alo + g and bhi != blo + g)
                    {
                        bits |= KOS_ASPACE_SPLIT_NONADJACENT;
                    }
                    // Written a byte at a time and never through the seam under test.
                    for (size_t i = 0; i < HALF; i++)
                    {
                        alo[g - HALF + i] = 0;
                        ahi[i] = 0;
                        spill[i] = 0;
                        blo[g - HALF + i] = static_cast<unsigned char>(0xB0u + i);
                        bhi[i] = static_cast<unsigned char>(0xC0u + i);
                    }
                    unsigned char pat[2 * HALF];
                    for (size_t i = 0; i < sizeof(pat); i++)
                    {
                        pat[i] = static_cast<unsigned char>(0x40u + i);
                    }
                    kaccess_to_user(sa, cross, pat, sizeof(pat));
                    bool landed = true;
                    bool untouched = true;
                    for (size_t i = 0; i < HALF; i++)
                    {
                        landed = landed and alo[g - HALF + i] == pat[i];
                        landed = landed and ahi[i] == pat[HALF + i];
                        untouched = untouched and spill[i] == 0;
                    }
                    if (landed)
                    {
                        bits |= KOS_ASPACE_SPLIT_TO_USER;
                    }
                    if (untouched)
                    {
                        bits |= KOS_ASPACE_SPLIT_NEIGHBOUR;
                    }
                    unsigned char back[2 * HALF];
                    for (size_t i = 0; i < sizeof(back); i++)
                    {
                        back[i] = 0;
                    }
                    kaccess_from_user(back, sa, cross, sizeof(back));
                    bool read_both = true;
                    for (size_t i = 0; i < HALF; i++)
                    {
                        read_both = read_both and back[i] == alo[g - HALF + i];
                        read_both = read_both and back[HALF + i] == ahi[i];
                    }
                    if (read_both)
                    {
                        bits |= KOS_ASPACE_SPLIT_FROM_USER;
                    }
                    // One address, two spaces, and the range straddles the boundary in
                    // both. The destination's numbers equal the source's, which a
                    // per-process image makes ordinary.
                    ep_copy(sa, cross, sb, cross, 2 * HALF);
                    bool crossed = true;
                    for (size_t i = 0; i < HALF; i++)
                    {
                        crossed = crossed and alo[g - HALF + i] == blo[g - HALF + i];
                        crossed = crossed and ahi[i] == bhi[i];
                        crossed = crossed and spill[i] == 0;
                    }
                    if (crossed)
                    {
                        bits |= KOS_ASPACE_SPLIT_CROSS_SPACE;
                    }
                }
                arch_aspace_release(sa, VA_A);
                arch_aspace_release(sa, VA_A + g);
                arch_aspace_release(sb, VA_A);
                arch_aspace_release(sb, VA_A + g);
            }
            // Unmapped and freed here rather than left to destroy: a map that failed leaves
            // its frame unmapped, so the space cannot be the one owner of all six.
            if (sa != nullptr)
            {
                (void)arch_aspace_unmap(sa, VA_A, 1);
                (void)arch_aspace_unmap(sa, VA_A + g, 1);
            }
            if (sb != nullptr)
            {
                (void)arch_aspace_unmap(sb, VA_A, 1);
                (void)arch_aspace_unmap(sb, VA_A + g, 1);
            }
            for (size_t i = 0; i < 3; i++)
            {
                arch_phys_addr_t const off = static_cast<arch_phys_addr_t>(i * g);
                if (ra != 0)
                {
                    kickos_frame_free(ra + off);
                }
                if (rb != 0)
                {
                    kickos_frame_free(rb + off);
                }
            }
            arch_aspace_destroy(sa);
            arch_aspace_destroy(sb);
            if (frame_pool_refused() == 0 and frame_pool_free() == before)
            {
                bits |= KOS_ASPACE_SPLIT_BALANCED;
            }
            return bits;
        }

        // The fourth transition, taken by the CPU rather than by a walk of our own: with the
        // space active, an ordinary load of a page just unmapped must fault. Nothing here
        // returns on a working backend.
        uint64_t op_touch_unmapped()
        {
            struct arch_aspace* const space = arch_aspace_create();
            if (space == nullptr)
            {
                return 0;
            }
            arch_phys_addr_t const frame = kickos_frame_alloc();
            if (frame == 0 or
                arch_aspace_map(space, VA_A, frame, 1, ARCH_MAP_R | ARCH_MAP_W,
                                ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
            {
                arch_aspace_destroy(space);
                return 0;
            }
            void* const seed = arch_aspace_acquire(space, VA_A);
            if (seed == nullptr)
            {
                arch_aspace_destroy(space);
                return 0;
            }
            *word_at(seed) = PATTERN_B;

            // Announced before the switch, because this space maps no console: the gate
            // compares this address against the FAR the dump reports, so the arm asserts
            // which page faulted rather than only that something did.
            kprintf("[aspace] unmapped 0x%lx, expecting a translation fault\n",
                    static_cast<unsigned long>(VA_A));

            // Masked throughout: the console and the interrupt controller are low addresses
            // on this chip and this space maps neither, so any ISR taken here would run
            // against a space that cannot reach them.
            IrqLock lock;
            // The switch path caches which space is installed, and this activates one behind
            // its back, so the cache is dropped rather than left naming a space that is no
            // longer the root.
            aspace_forget_current();
            arch_aspace_activate(space);
            uint32_t const seen = *word_at(reinterpret_cast<void*>(VA_A));
            (void)arch_aspace_unmap(space, VA_A, 1);
            uint32_t const after = *word_at(reinterpret_cast<void*>(VA_A));
            // Reached only where the load above did not fault: a backend whose unmap left
            // the translation standing answers a value here instead of a dump.
            //
            // Back to the caller's own space, not to the boot one: the caller returns to app
            // text its own root maps, and the boot map is not that root.
            aspace_activate_for(sched::current());
            arch_aspace_destroy(space);
            kickos_frame_free(frame);
            if (seen != PATTERN_B)
            {
                return 1; // the running translation never reached the frame
            }
            return 2 + static_cast<uint64_t>(after == PATTERN_B);
        }

        size_t frames_lost(size_t before)
        {
            size_t const after = frame_pool_free();
            if (after >= before)
            {
                return 0;
            }
            return before - after;
        }

        // --- The forced failure, swept one allocation at a time ---------------------
        // The instrument is frame_pool_fail_in, and the sweep walks the refused attempt from
        // the first allocation a create makes to past its last.
        //
        // Measured immediately after each refusal: a refusal that left the slot holding a
        // half-built space balances anyway once the next claim_slot releases it, so a
        // balance read at the end of the sweep cannot see it.
        //
        // The refusal counter carries the other half. A leaf left standing over a frame the
        // unwind already returned is not a frame delta; it is a second free when destroy
        // walks the tree, and the pool refuses that rather than swallowing it.
        //
        // `donor_base` at 0 sweeps the no-grant create, which is claim_slot alone. Anything
        // else names a range the calling task reserved and sweeps the grant-carrying create
        // instead. The size is taken from the caller's own list rather than from the caller,
        // so a number nobody reserved names no sweep.
        uint64_t op_forced_unwind(uintptr_t donor_base)
        {
            IrqLock lock;
            Domain* donor = nullptr;
            void* mem_base = nullptr;
            size_t mem_size = 0;
            if (donor_base != 0)
            {
                Thread const* const c = sched::current();
                if (c == nullptr)
                {
                    return 0;
                }
                donor = task_domain(c->task);
                VirtualRanges const* const r = domain_ranges(donor);
                if (r == nullptr)
                {
                    return 0;
                }
                VirtualRange const* const e = r->find(donor_base, 1);
                if (e == nullptr)
                {
                    return 0;
                }
                mem_base = reinterpret_cast<void*>(e->base);
                mem_size = e->pages * arch_aspace_granule();
            }
            // Far past the allocations a create makes, so the sweep ends by running out of
            // injection points rather than by running out of sweep.
            constexpr size_t SWEEP_LIMIT = 64;
            size_t const before = frame_pool_free();
            size_t const refused_before = frame_pool_refused();
            uint64_t bits = KOS_ASPACE_UNWIND_REFUSED | KOS_ASPACE_UNWIND_ENOMEM
                            | KOS_ASPACE_UNWIND_BALANCED;
            size_t depth = 0;
            for (size_t nth = 1; nth <= SWEEP_LIMIT; nth++)
            {
                frame_pool_fail_in(nth);
                int derr = 0;
                Domain* const d = domain_for(DOM_CALLER_MEM_AUTH, mem_base,
                                             mem_size, 0, donor, &derr);
                bool const spent = not frame_pool_fail_armed();
                frame_pool_fail_in(0);
                if (not spent)
                {
                    // The create finished with the arming untouched, so `nth` is past its
                    // last allocation and every point before this one has been injected.
                    bits |= KOS_ASPACE_UNWIND_SWEPT;
                    if (d != nullptr)
                    {
                        domain_release(d);
                        if (frame_pool_free() == before)
                        {
                            bits |= KOS_ASPACE_UNWIND_REUSABLE;
                        }
                    }
                    break;
                }
                depth++;
                if (d != nullptr)
                {
                    // A refused allocation the create carried on past: the arm under it is
                    // not total-or-fail.
                    domain_release(d);
                    bits &= ~static_cast<uint64_t>(KOS_ASPACE_UNWIND_REFUSED);
                    continue;
                }
                if (derr != KOS_ENOMEM)
                {
                    bits &= ~static_cast<uint64_t>(KOS_ASPACE_UNWIND_ENOMEM);
                }
                if (frame_pool_free() != before)
                {
                    bits &= ~static_cast<uint64_t>(KOS_ASPACE_UNWIND_BALANCED);
                }
            }
            if (frame_pool_refused() == refused_before)
            {
                bits |= KOS_ASPACE_UNWIND_NO_DOUBLE;
            }
            return bits | (static_cast<uint64_t>(depth) << KOS_ASPACE_UNWIND_DEPTH_SHIFT);
        }

        // A domain that is resolved and dropped must hand the root and its tables back. Two
        // ways it does not, and they mask each other under one measurement because both free
        // the same table: a release that only decrements, and a free slot reused while its
        // predecessor's space still stands, which is the shape task_for leaves when a spawn
        // fails after the domain resolved. So each is measured over a sequence the other
        // cannot repair.
        uint64_t op_domain_balance()
        {
            IrqLock lock;
            // Resolves with no reference, all landing on one free slot, so every resolve
            // after the first must return what the last one left.
            size_t const before_reuse = frame_pool_free();
            Domain* last = nullptr;
            for (int i = 0; i < 4; i++)
            {
                int derr = 0;
                Domain* const d = domain_for(DOM_CALLER_MEM_AUTH, nullptr, 0, 0, nullptr, &derr);
                if (d == nullptr)
                {
                    return 0; // no frame for a root: nothing to weigh
                }
                last = d;
            }
            domain_release(last);
            size_t const lost_reuse = frames_lost(before_reuse);
            // The release itself, weighed from after the resolve: measured over a resolve
            // and a release together, the reuse cleanup above would hand back exactly the
            // space a missing release destroy kept, and the pair would read as balanced
            // while every dead process held its tables until its slot was next claimed.
            int derr = 0;
            Domain* const one = domain_for(DOM_CALLER_MEM_AUTH, nullptr, 0, 0, nullptr, &derr);
            if (one == nullptr)
            {
                return 0;
            }
            size_t const held = frame_pool_free();
            domain_release(one);
            size_t const returned = frame_pool_free();
            if (frame_pool_refused() != 0)
            {
                return ~static_cast<uint64_t>(0);
            }
            if (returned > held)
            {
                return lost_reuse;
            }
            return lost_reuse + 1u;
        }
    }

    // The model word crosses the ABI unchanged, so the two vocabularies must agree bit for bit.
    static_assert(KOS_ASPACE_MODEL_GRANULE == ARCH_ASPACE_MODEL_GRANULE, "model bit drift");
    static_assert(KOS_ASPACE_MODEL_ASID == ARCH_ASPACE_MODEL_ASID, "model bit drift");
    static_assert(KOS_ASPACE_MODEL_PA == ARCH_ASPACE_MODEL_PA, "model bit drift");
    static_assert(KOS_ASPACE_MODEL_ASID_SHIFT == ARCH_ASPACE_MODEL_ASID_SHIFT, "model bit drift");
    static_assert(KOS_ASPACE_MODEL_PA_SHIFT == ARCH_ASPACE_MODEL_PA_SHIFT, "model bit drift");
    static_assert(KOS_ASPACE_MODEL_GRAN_SHIFT == ARCH_ASPACE_MODEL_GRAN_SHIFT, "model bit drift");

    uint64_t aspace_probe(uintptr_t op, uintptr_t a1)
    {
        switch (op)
        {
            case KOS_ASPACE_OP_GRANULE:
            {
                return arch_aspace_granule();
            }
            case KOS_ASPACE_OP_MEMTYPE:
            {
                return static_cast<uint64_t>(
                    arch_aspace_memtype_support(static_cast<enum arch_map_memtype>(a1)));
            }
            case KOS_ASPACE_OP_FRAMES_FREE:
            {
                return frame_pool_free();
            }
            case KOS_ASPACE_OP_ROUNDTRIP:
            {
                return op_roundtrip();
            }
            case KOS_ASPACE_OP_ALIAS:
            {
                return op_alias();
            }
            case KOS_ASPACE_OP_REFUSALS:
            {
                return op_refusals();
            }
            case KOS_ASPACE_OP_BALANCE:
            {
                return op_balance();
            }
            case KOS_ASPACE_OP_TOUCH_UNMAPPED:
            {
                return op_touch_unmapped();
            }
            case KOS_ASPACE_OP_SPAN:
            {
                return op_span();
            }
            case KOS_ASPACE_OP_SPLIT_ACCESS:
            {
                return op_split_access();
            }
            case KOS_ASPACE_OP_DOMAIN_BALANCE:
            {
                return op_domain_balance();
            }
            case KOS_ASPACE_OP_FORCED_UNWIND:
            {
                return op_forced_unwind(a1);
            }
            case KOS_ASPACE_OP_SPACES_HELD:
            {
                return domain_spaces_held();
            }
            case KOS_ASPACE_OP_RANGES_FREE:
            {
                Thread const* const c = sched::current();
                if (c == nullptr)
                {
                    return 0;
                }
                VirtualRanges const* const r = domain_ranges(task_domain(c->task));
                if (r == nullptr)
                {
                    return 0;
                }
                return VirtualRanges::capacity() - r->count();
            }
            case KOS_ASPACE_OP_FRAME_AT:
            {
                // Two tasks comparing this for one address is what witnesses that
                // per-process static data is a copy (section 3.4).
                Thread const* const c = sched::current();
                if (c == nullptr)
                {
                    return 0;
                }
                return aspace_frame_token(domain_space(task_domain(c->task)), a1);
            }
            case KOS_ASPACE_OP_MODEL:
            {
                // Passed straight through: the asserts above are what keep the two
                // vocabularies one word.
                return arch_aspace_model();
            }
            case KOS_ASPACE_OP_MEMTYPE_AT:
            {
                Thread const* const c = sched::current();
                if (c == nullptr)
                {
                    return 0;
                }
                VirtualRanges const* const r = domain_ranges(task_domain(c->task));
                if (r == nullptr)
                {
                    return 0;
                }
                VirtualRange const* const e = r->find(a1, 1);
                if (e == nullptr or e->state != VirtualState::Granted)
                {
                    return 0;
                }
                return static_cast<uint64_t>(e->memtype) + 1u;
            }
            case KOS_ASPACE_OP_ACQUIRE_BALANCE:
            {
                return aspace_acquire_balance();
            }
            case KOS_ASPACE_OP_REENT_SEATING:
            {
                // Two halves, and the arm needs both: bit 0 says the posture is reached at
                // all, bit 1 says the app half was written in it. A guard that stops guarding
                // lights bit 1; a posture that stopped happening drops bit 0 and would leave
                // bit 1 vacuously clear.
                uint64_t out = 0;
                if (aspace_unseated_switch_ins() != 0)
                {
                    out |= 1u;
                }
                if (reent_unseated_writes() != 0)
                {
                    out |= 2u;
                }
                return out;
            }
            case KOS_ASPACE_OP_DATA_HOME_FORGET:
            {
                aspace_data_home_forget();
                return 0;
            }
            case KOS_ASPACE_OP_MAP_TLBI:
            {
                return arch_aspace_tlbi_counts();
            }
            case KOS_ASPACE_OP_SPACE_ID:
            {
                // Two tasks comparing this is what witnesses that a domain is an address
                // space of its own (docs/design-m6-mmu.md F2).
                Thread const* const c = sched::current();
                if (c == nullptr)
                {
                    return 0;
                }
                return domain_space_id(task_domain(c->task));
            }
            default:
            {
                break;
            }
        }
        return static_cast<uint64_t>(-KOS_EINVAL);
    }
}

#endif
