// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test scaffolding for the address-space seam (KOS_SYS_ASPACE_PROBE). The map editor is a
// KERNEL seam and never a syscall, so each op runs a whole scenario here and answers a
// number; nothing hands userspace a mapping primitive.
//
// THE ADDRESSES ARE DELIBERATELY NOT THE FRAME'S. An identity map answers every arm of this
// step correctly, so a scenario that mapped a frame at its own output address would prove
// only that the memory exists (docs/design-m6-mmu.md section 3.2).

#include <kickos/arch/arch.h>

#include "syscall_internal.h"

#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)

#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
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

        // map, write, read back, unmap, and the page gone: the four transitions, reached
        // through the acquire pair rather than through the running translation, so an arm
        // that fails leaves the caller alive to report it.
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
                // Destroy reclaims what a space still HOLDS, so a frame no leaf points at
                // when it runs is the caller's to return.
                kickos_frame_free(frame);
            }
            return trip;
        }

        // Two unequal virtual pages onto the one frame the caller chose, then a write
        // through one seen through the other, and the frame's own bytes agreeing. An
        // identity map cannot produce this: it would give two different pages.
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
                // same frame, and destroy frees what a space MAPS.
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

        // Every frame a whole cycle took must come back, tables included. A build with no
        // destroy walk passes every other arm of this step while leaking each space.
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
                // The leaf still mapped at VA_A is the space's to reclaim, which is what
                // makes destroy rather than unmap the owner of a frame's life.
                arch_aspace_destroy(space);
            }
            // EVERY op, not just this cycle: no scenario in this file may hand the pool a
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
        // The output addresses are DEVICE space, so no leaf here names a pool frame and the
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
                unsigned char* const first =
                    static_cast<unsigned char*>(arch_aspace_acquire(space, SPAN_VA));
                bool contiguous = first != nullptr;
                for (size_t i = 1; i < SPAN_PAGES and contiguous; i++)
                {
                    unsigned char* const at = static_cast<unsigned char*>(
                        arch_aspace_acquire(space, SPAN_VA + i * g));
                    contiguous = at == first + i * g;
                }
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

        // The fourth transition, taken by the CPU rather than by a walk of our own: with the
        // space ACTIVE, an ordinary load of a page just unmapped must fault. Nothing here
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

            // ANNOUNCED BEFORE THE SWITCH, because this space maps no console: the gate
            // compares this address against the FAR the dump reports, so the arm asserts
            // WHICH page faulted rather than only that something did.
            kprintf("[aspace] unmapped 0x%lx, expecting a translation fault\n",
                    static_cast<unsigned long>(VA_A));

            // Masked throughout: the console and the interrupt controller are LOW addresses
            // on this chip and this space maps neither, so any ISR taken here would run
            // against a space that cannot reach them.
            IrqLock lock;
            arch_aspace_activate(space);
            uint32_t const seen = *word_at(reinterpret_cast<void*>(VA_A));
            (void)arch_aspace_unmap(space, VA_A, 1);
            uint32_t const after = *word_at(reinterpret_cast<void*>(VA_A));
            // Reached only where the load above did NOT fault, which is the failure this arm
            // exists to catch: a backend whose unmap left the translation standing answers a
            // value here instead of a dump.
            arch_aspace_activate(arch_aspace_boot());
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

        // A domain carries an address space where a backend translates, so a domain that is
        // resolved and dropped must hand the root and its tables back. TWO ways it does not,
        // and they MASK EACH OTHER under one measurement because both free the same table:
        // a release that only decrements, and a FREE SLOT reused while its predecessor's
        // space still stands, which is the shape task_for leaves when a spawn fails after
        // the domain resolved. So each is measured over a sequence the other cannot repair.
        uint64_t op_domain_balance()
        {
            IrqLock lock;
            // Resolves with NO reference, all landing on one free slot, so every resolve
            // after the first must return what the last one left.
            size_t const before_reuse = frame_pool_free();
            Domain* last = nullptr;
            for (int i = 0; i < 4; i++)
            {
                int derr = 0;
                Domain* const d = domain_for(false, nullptr, 0, 0, true, &derr);
                if (d == nullptr)
                {
                    return 0; // no frame for a root: nothing to weigh
                }
                last = d;
            }
            domain_release(last);
            size_t const lost_reuse = frames_lost(before_reuse);
            // The release itself, weighed from AFTER the resolve: measured over a resolve
            // and a release together, the reuse cleanup above would hand back exactly the
            // space a missing release destroy kept, and the pair would read as balanced
            // while every dead process held its tables until its slot was next claimed.
            int derr = 0;
            Domain* const one = domain_for(false, nullptr, 0, 0, true, &derr);
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
            case KOS_ASPACE_OP_DOMAIN_BALANCE:
            {
                return op_domain_balance();
            }
            case KOS_ASPACE_OP_SPACE_ID:
            {
                // The one op that answers about the CALLER rather than running a scenario:
                // two tasks comparing this is what witnesses that a domain is an address
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
