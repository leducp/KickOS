// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test scaffolding for the address-space seam (KOS_SYS_ASPACE_PROBE). Each op runs a whole
// scenario and answers a number; nothing hands userspace a mapping primitive. The addresses are
// not the frame's, so no arm can pass on an identity map alone.

#include <kickos/arch/arch.h>

#include "syscall_internal.h"

#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)

#include <kickos/ampwindow.h>
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

#if KICKOS_AMP_NODE
        uint64_t sat8(uint32_t v)
        {
            uint64_t o = v;
            if (v > 0xFFu)
            {
                o = 0xFFu;
            }
            return o;
        }

        uint64_t amp_counts_packed(uint32_t node)
        {
            amp::Counts const& c = amp::counts(node);
            uint64_t serviced = c.serviced;
            if (c.serviced > 0xFFFFu)
            {
                serviced = 0xFFFFu;
            }
            return (serviced << 48) | (sat8(c.send_refused) << 40) | (sat8(c.sent) << 32)
                   | (sat8(c.port) << 24) | (sat8(c.length) << 16) | (sat8(c.depth) << 8)
                   | sat8(c.took);
        }

        constexpr uint32_t ROUND_LEN = 16u;

        uint64_t amp_round(uint32_t node)
        {
            uint8_t payload[ROUND_LEN];
            for (uint32_t i = 0; i < ROUND_LEN; i++)
            {
                payload[i] = static_cast<uint8_t>(0xA0u + i);
            }
            return static_cast<uint64_t>(
                static_cast<uint32_t>(amp::send(node, amp::PORT_ECHO, payload, ROUND_LEN)));
        }

        uint64_t verdict_code(amp::Verdict v)
        {
            if (v == amp::Verdict::TOOK)
            {
                return KOS_AMP_V_TOOK;
            }
            if (v == amp::Verdict::DEPTH)
            {
                return KOS_AMP_V_DEPTH;
            }
            if (v == amp::Verdict::LENGTH)
            {
                return KOS_AMP_V_LENGTH;
            }
            if (v == amp::Verdict::PORT)
            {
                return KOS_AMP_V_PORT;
            }
            return KOS_AMP_V_EMPTY;
        }

        constexpr uint32_t FORGE_FROM = 1u;

        uint64_t send_code(amp::Sent rc)
        {
            if (rc == amp::Sent::OK)
            {
                return KOS_AMP_V_SEND_OK;
            }
            if (rc == amp::Sent::DEPTH)
            {
                return KOS_AMP_V_SEND_DEPTH;
            }
            if (rc == amp::Sent::NODE)
            {
                return KOS_AMP_V_SEND_NODE;
            }
            return KOS_AMP_V_SEND_REFUSED;
        }

        uint64_t amp_forge(uint32_t selector)
        {
            if (selector == KOS_AMP_FORGE_TAIL_DEPTH)
            {
                return send_code(amp::forge_tail_and_send(FORGE_FROM, amp::RING_SLOTS + 1u));
            }
            if (selector == KOS_AMP_FORGE_SELF_SEND)
            {
                // The caller's own node, named through the real send rather than a forge: no
                // far side is involved and nothing has to be malformed.
                uint8_t byte = 0;
                return send_code(
                    amp::send(arch_cpu_id(), amp::PORT_ECHO, &byte, sizeof(byte)));
            }
            if (selector == KOS_AMP_FORGE_DEPTH_RESET)
            {
                return verdict_code(amp::forge_depth_recovery(FORGE_FROM));
            }

            uint32_t port = amp::PORT_ECHO;
            uint32_t len = ROUND_LEN;
            uint32_t head_jump = 0;
            if (selector == KOS_AMP_FORGE_HEAD_DEPTH)
            {
                head_jump = amp::RING_SLOTS + 1u;
            }
            else if (selector == KOS_AMP_FORGE_LENGTH)
            {
                len = amp::SLOT_BYTES + 1u;
            }
            else if (selector == KOS_AMP_FORGE_PORT)
            {
                // Inside the mint's width and never minted, which a width check alone passes.
                port = amp::PORT_MAX - 1u;
            }
            else if (selector == KOS_AMP_FORGE_PORT_WIDE)
            {
                port = amp::PORT_MAX;
            }
            else if (selector == KOS_AMP_FORGE_ZERO_LEN)
            {
                len = 0;
            }
            else if (selector != KOS_AMP_FORGE_WELL_FORMED)
            {
                return KOS_AMP_V_EMPTY;
            }
            return verdict_code(amp::forge_and_take(FORGE_FROM, port, len, head_jump));
        }
#endif

        // Spelled out rather than __builtin_popcount, which lowers to a libgcc call on a part
        // without the instruction.
        uint32_t popcount32(uint32_t v)
        {
            uint32_t n = 0;
            while (v != 0)
            {
                v &= v - 1u;
                n++;
            }
            return n;
        }

        // Whether the page is reachable, with the hold surrendered: an acquire that answered
        // took a hold whether the arm wanted the pointer or not, and arch.h counts calls.
        bool acquire_answers(struct arch_aspace* space, uintptr_t va)
        {
            void* const p = arch_aspace_acquire(space, va);
            if (p == nullptr)
            {
                return false;
            }
            arch_aspace_release(space, va);
            return true;
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
                    if (not acquire_answers(space, VA_A))
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
                // The seam names the frame behind each page, so this compares frames: a
                // windowed backend hands back two unequal slot addresses for one frame.
                bool const same_frame = arch_aspace_frame_at(space, VA_A) == frame
                                        and arch_aspace_frame_at(space, VA_B) == frame;
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
                if (a != nullptr)
                {
                    arch_aspace_release(space, VA_A);
                }
                if (b != nullptr)
                {
                    arch_aspace_release(space, VA_B);
                }
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
            // A run whose last page is the first one past the output-address width, based on
            // the last page the machine can output. The width is the port's own report, and a
            // port reporting none is not asked. Both pages are checked for absence: the seam's
            // rule is total-or-fail, and a leaf carrying a truncated output answers 0 exactly as
            // an unmapped page does.
            unsigned const pa_bits = static_cast<unsigned>(
                (arch_aspace_model() >> ARCH_ASPACE_MODEL_PA_SHIFT)
                & ARCH_ASPACE_MODEL_FIELD_MASK);
            if (pa_bits != 0 and pa_bits < 64)
            {
                arch_phys_addr_t const top = (static_cast<arch_phys_addr_t>(1) << pa_bits)
                                             - static_cast<arch_phys_addr_t>(g);
                enum arch_aspace_result const rc =
                    arch_aspace_map(space, VA_B, top, 2, rw, ARCH_MAP_NORMAL);
                if (rc != ARCH_ASPACE_OK and arch_aspace_frame_at(space, VA_B) == 0
                    and arch_aspace_frame_at(space, VA_B + g) == 0
                    and not acquire_answers(space, VA_B))
                {
                    bits |= KOS_ASPACE_REFUSE_PHYS_EXTENT;
                }
                if (rc == ARCH_ASPACE_OK)
                {
                    // The leaves name frames the pool never handed out, so they go before the
                    // destroy that would offer them back to it.
                    (void)arch_aspace_unmap(space, VA_B, 2);
                }
            }
            // One page mapped of a two-page unmap: the range is not wholly mapped, so the
            // call must refuse and clear nothing.
            if (arch_aspace_map(space, VA_A, frame, 1, rw, ARCH_MAP_NORMAL) == ARCH_ASPACE_OK)
            {
                if (arch_aspace_unmap(space, VA_A, 2) != ARCH_ASPACE_OK and
                    acquire_answers(space, VA_A))
                {
                    bits |= KOS_ASPACE_REFUSE_PART_UNMAP;
                }
            }
            arch_aspace_destroy(space);
            return bits;
        }

        // Every frame a whole cycle took must come back, tables included.
        // The two kinds resolve through the same chokepoint every other kind does.
        // Nothing here composes an address; only yes/no bits cross back.
        uint64_t op_cap_objects()
        {
            IrqLock lock;
            Thread* self = sched::current();
            if (self == nullptr)
            {
                return 0;
            }
            uint64_t bits = 0;
            size_t const free_before = frame_pool_free();
            // A double free does not move the free count: the pool refuses and counts it.
            size_t const refused_before = frame_pool_refused();
            Domain* const mine = task_domain(self->task);
            uint16_t const hold_before = domain_refcount(mine);

            // A frame RUN, taken from the pool and named by a capability.
            size_t const g = arch_aspace_granule();
            arch_phys_addr_t const run = frame_pool_alloc_user_run(2);
            uint32_t fcap = KCAP_INVALID;
            int fobj = -1;
            if (run != 0)
            {
                fobj = frame_run_create(run, 2);
                if (fobj >= 0)
                {
                    if (cap_install(self, fobj, CapType::CAP_FRAME, CAP_TRANSFER, &fcap) == 0)
                    {
                        bits |= KOS_ASPACE_CAPOBJ_FRAME_MINT;
                    }
                    else
                    {
                        // Surrendering the run returns the frames AND the slot. Leave fobj
                        // set: the tail's pool-free is for a run that never became an object,
                        // and clearing it here frees the frames a second time.
                        fcap = KCAP_INVALID;
                        frame_run_release(fobj);
                    }
                }
            }
            if (fcap != KCAP_INVALID)
            {
                FrameRun* got = static_cast<FrameRun*>(
                    cap_resolve(self, fcap, CapType::CAP_FRAME, 0));
                if (got != nullptr and got->base == run and got->pages == 2)
                {
                    bits |= KOS_ASPACE_CAPOBJ_FRAME_RESOLVE;
                }
            }

            // The address space this task already holds, named by a capability.
            uint32_t acap = KCAP_INVALID;
            if (mine != nullptr)
            {
                int const h = domain_handle(mine);
                if (obj_ref_inc(CapType::CAP_ASPACE, h, 0))
                {
                    if (cap_install(self, h, CapType::CAP_ASPACE, CAP_TRANSFER, &acap) != 0)
                    {
                        acap = KCAP_INVALID;
                        obj_ref_undo(CapType::CAP_ASPACE, h, 0);
                    }
                    else
                    {
                        bits |= KOS_ASPACE_CAPOBJ_ASPACE_MINT;
                        if (domain_refcount(mine) == hold_before + 1u)
                        {
                            bits |= KOS_ASPACE_CAPOBJ_ASPACE_HOLD;
                        }
                        if (cap_resolve(self, acap, CapType::CAP_ASPACE, 0) == mine)
                        {
                            bits |= KOS_ASPACE_CAPOBJ_ASPACE_STALE; // provisional, cleared below
                        }
                    }
                }
            }
            // The generation is what makes a reclaimed slot unreachable, so the stale bit is
            // earned by a handle whose generation has MOVED, not by one that still resolves.
            if ((bits & KOS_ASPACE_CAPOBJ_ASPACE_STALE) != 0 and mine != nullptr)
            {
                int const stale = domain_handle(mine) + (1 << 16); // one generation on
                if (domain_resolve(stale) != nullptr)
                {
                    bits &= ~static_cast<uint64_t>(KOS_ASPACE_CAPOBJ_ASPACE_STALE);
                }
            }

            if (fcap != KCAP_INVALID)
            {
                handle_close(self, fcap);
                if (frame_pool_free() == free_before)
                {
                    bits |= KOS_ASPACE_CAPOBJ_CLOSE_FRAMES;
                }
            }
            else if (run != 0 and fobj < 0)
            {
                // Only where no run object was seated: once one is, its release returns these.
                frame_pool_free_run(run, 2, g);
            }
            if (acap != KCAP_INVALID)
            {
                handle_close(self, acap);
                if (domain_refcount(mine) == hold_before)
                {
                    bits |= KOS_ASPACE_CAPOBJ_CLOSE_HOLD;
                }
            }
            if (frame_pool_free() == free_before and domain_refcount(mine) == hold_before)
            {
                bits |= KOS_ASPACE_CAPOBJ_BALANCED;
            }
            if (frame_pool_refused() == refused_before)
            {
                bits |= KOS_ASPACE_CAPOBJ_NO_REFUSED;
            }
            return bits;
        }

        // A LOW-HALF ADDRESS FAR FROM DRAM, and never a physical base out of the frame pool:
        // that window is where ustack_alloc maps every thread stack, so an address inside it
        // is one a CHILD's stack may take even where nothing in this space names it yet.
        constexpr uintptr_t VA_SEED = 0x14000000;
        // Pages per candidate: a holder adds offsets to pick an address of its own and must
        // stay inside the window this checked.
        constexpr size_t VA_SEED_PAGES = 16u;

        int g_seed_obj = -1;

        uint64_t op_cap_seed()
        {
            IrqLock lock;
            Thread* self = sched::current();
            Domain* const mine = (self == nullptr) ? nullptr : task_domain(self->task);
            if (self == nullptr or mine == nullptr)
            {
                return 0;
            }
            // Cleared: alloc_run hands out the previous owner's bytes and this run crosses
            // into another task.
            arch_phys_addr_t const run = frame_pool_alloc_user_run(1);
            if (run == 0)
            {
                return 0;
            }
            int const fobj = frame_run_create(run, 1);
            if (fobj < 0)
            {
                frame_pool_free_run(run, 1, arch_aspace_granule());
                return 0;
            }
            uint32_t fcap = KCAP_INVALID;
            if (cap_install(self, fobj, CapType::CAP_FRAME, CAP_TRANSFER, &fcap) != 0)
            {
                // The RUN OBJECT exists and holds the creator's reference: surrendering it is
                // what returns the frames AND the slot. Freeing the frames alone strands it.
                frame_run_release(fobj);
                return 0;
            }
            int const h = domain_handle(mine);
            uint32_t acap = KCAP_INVALID;
            if (not obj_ref_inc(CapType::CAP_ASPACE, h, 0))
            {
                handle_close(self, fcap); // no hold was taken, so none is undone
                return 0;
            }
            if (cap_install(self, h, CapType::CAP_ASPACE, CAP_TRANSFER, &acap) != 0)
            {
                obj_ref_undo(CapType::CAP_ASPACE, h, 0);
                handle_close(self, fcap);
                return 0;
            }
            g_seed_obj = fobj;
            return (static_cast<uint64_t>(acap) << 32) | static_cast<uint64_t>(fcap);
        }

        uint64_t op_cap_self_space()
        {
            IrqLock lock;
            Thread* self = sched::current();
            Domain* const mine = (self == nullptr) ? nullptr : task_domain(self->task);
            if (self == nullptr or mine == nullptr)
            {
                return 0;
            }
            int const h = domain_handle(mine);
            uint32_t acap = KCAP_INVALID;
            if (not obj_ref_inc(CapType::CAP_ASPACE, h, 0))
            {
                return 0;
            }
            if (cap_install(self, h, CapType::CAP_ASPACE, CAP_TRANSFER, &acap) != 0)
            {
                obj_ref_undo(CapType::CAP_ASPACE, h, 0);
                return 0;
            }
            return static_cast<uint64_t>(acap);
        }

        uint64_t op_cap_seed_va()
        {
            IrqLock lock;
            Thread const* const c = sched::current();
            size_t const g = arch_aspace_granule();
            if (c == nullptr or g == 0)
            {
                return 0;
            }
            VirtualRanges const* const r = domain_ranges(task_domain(c->task));
            if (r == nullptr)
            {
                return static_cast<uint64_t>(VA_SEED);
            }
            // ASKABLE ONLY BECAUSE THE LIST IS TOTAL: thread stacks and their guards are
            // recorded, so "an address nothing in this space names" is a question it answers.
            for (unsigned i = 0; i < 8u; i++)
            {
                uintptr_t const va =
                    VA_SEED + static_cast<uintptr_t>(i) * static_cast<uintptr_t>(VA_SEED_PAGES * g);
                if (not r->overlaps(va, VA_SEED_PAGES))
                {
                    return static_cast<uint64_t>(va);
                }
            }
            return 0;
        }

        uint64_t op_cap_run_refs()
        {
            IrqLock lock;
            return static_cast<uint64_t>(frame_run_refcount(g_seed_obj));
        }

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

        // A range that crosses two last-level table boundaries, which is what a process image is.
        // The output addresses are device space, so no leaf here names a pool frame. No table
        // geometry is named: an entry is at least four bytes on every backend the seam is held
        // against, so one table covers at most granule/4 pages, and a run one page longer entered
        // one page below a boundary of that span crosses two boundaries whatever the geometry is.
        uint64_t op_span()
        {
            constexpr arch_phys_addr_t SPAN_PA = 0x08000000;
            constexpr uintptr_t SPAN_NEAR = 0x20000000; // a low-half range a fresh space maps nowhere
            struct arch_aspace* const space = arch_aspace_create();
            if (space == nullptr)
            {
                return 0;
            }
            size_t const g = arch_aspace_granule();
            size_t const per_table = g / 4;
            uintptr_t const table_span = static_cast<uintptr_t>(per_table) * g;
            uintptr_t const span_va = (SPAN_NEAR & ~(table_span - 1)) + table_span - g;
            size_t const span_pages = per_table + 88; // one, then a whole table, then 87
            uint64_t ok = 0;
            if (arch_aspace_map(space, span_va, SPAN_PA, span_pages, ARCH_MAP_R, ARCH_MAP_DEVICE) == ARCH_ASPACE_OK)
            {
                // Contiguity is asked of the FRAMES and access of the POINTERS: page i must
                // name SPAN_PA + i * granule, while the acquire beside it need only answer and
                // keep its offset inside the granule. Two held at a time, so this walk stays
                // inside ARCH_ASPACE_ACQUIRE_MIN.
                constexpr uintptr_t IN_PAGE = 0x40;
                unsigned char* const first = static_cast<unsigned char*>(arch_aspace_acquire(space, span_va));
                uintptr_t const mask = static_cast<uintptr_t>(g - 1u);
                bool contiguous = first != nullptr and arch_aspace_frame_at(space, span_va) == SPAN_PA;
                for (size_t i = 1; i < span_pages and contiguous; i++)
                {
                    uintptr_t const at_va = span_va + i * g + IN_PAGE;
                    unsigned char* const at = static_cast<unsigned char*>(arch_aspace_acquire(space, at_va));
                    contiguous = at != nullptr and arch_aspace_frame_at(space, at_va) == SPAN_PA + static_cast<arch_phys_addr_t>(i * g);
                    if (contiguous)
                    {
                        uintptr_t const p = reinterpret_cast<uintptr_t>(at);
                        contiguous = (p & mask) == IN_PAGE;
                    }
                    // Only where the acquire ANSWERED: a null answer took no hold, and a
                    // windowed backend refuses a release that pairs with none.
                    if (at != nullptr)
                    {
                        arch_aspace_release(space, at_va);
                    }
                }
                if (first != nullptr)
                {
                    arch_aspace_release(space, span_va);
                }
                bool const bounded =
                    arch_aspace_frame_at(space, span_va + span_pages * g) == 0 and
                    arch_aspace_frame_at(space, span_va - g) == 0 and
                    not acquire_answers(space, span_va + span_pages * g) and
                    not acquire_answers(space, span_va - g);
                bool const removed =
                    arch_aspace_unmap(space, span_va, span_pages) == ARCH_ASPACE_OK and
                    arch_aspace_frame_at(space, span_va) == 0 and
                    arch_aspace_frame_at(space, span_va + (span_pages - 1) * g) == 0 and
                    not acquire_answers(space, span_va) and
                    not acquire_answers(space, span_va + (span_pages - 1) * g);
                if (contiguous and bounded and removed)
                {
                    ok = 1;
                }
            }
            arch_aspace_destroy(space);
            return ok;
        }

        // Two simultaneous holds of one page, which is the shape a release cannot name: the seam
        // takes (space, va) and no token. The device output addresses put both pages outside any
        // kernel window, so a windowing backend spends real slots here. Nothing is read through
        // these pointers.
        uint64_t op_acquire_dup()
        {
            constexpr arch_phys_addr_t DUP_PA = 0x08000000;
            constexpr uintptr_t DUP_VA_A = 0x12000000;
            constexpr uintptr_t DUP_VA_B = 0x13000000;
            struct arch_aspace* const space = arch_aspace_create();
            if (space == nullptr)
            {
                return 0;
            }
            size_t const g = arch_aspace_granule();
            uint64_t bits = 0;
            if (arch_aspace_map(space, DUP_VA_A, DUP_PA, 1, ARCH_MAP_R, ARCH_MAP_DEVICE) ==
                    ARCH_ASPACE_OK and
                arch_aspace_map(space, DUP_VA_B, DUP_PA + static_cast<arch_phys_addr_t>(g), 1,
                                ARCH_MAP_R, ARCH_MAP_DEVICE) == ARCH_ASPACE_OK)
            {
                void* const a = arch_aspace_acquire(space, DUP_VA_A);
                void* const b = arch_aspace_acquire(space, DUP_VA_A);
                if (a != nullptr and b != nullptr and a == b)
                {
                    bits |= KOS_ASPACE_DUP_STABLE;
                }
                // One of the two holds goes back while the other is still outstanding, so the
                // next page acquired must not be answered with the address that hold names.
                if (b != nullptr)
                {
                    arch_aspace_release(space, DUP_VA_A);
                }
                void* const c = arch_aspace_acquire(space, DUP_VA_B);
                if (c != nullptr and a != nullptr and c != a)
                {
                    bits |= KOS_ASPACE_DUP_DISTINCT;
                }
                if (c != nullptr)
                {
                    arch_aspace_release(space, DUP_VA_B);
                }
                if (a != nullptr)
                {
                    arch_aspace_release(space, DUP_VA_A);
                }
                if (acquire_answers(space, DUP_VA_A))
                {
                    bits |= KOS_ASPACE_DUP_REUSABLE;
                }
                // Both leaves go before destroy walks the tree: their outputs are device
                // addresses the pool never handed out, and destroy frees what a space maps.
                (void)arch_aspace_unmap(space, DUP_VA_A, 1);
                (void)arch_aspace_unmap(space, DUP_VA_B, 1);
            }
            arch_aspace_destroy(space);
            return bits;
        }

        // --- The page split, built below the reservation API ------------------------
        // A validated range contiguous in virtual memory need not be contiguous in physical
        // memory, and a reservation's virtual address is its output address, so no such range
        // can be reserved. Built with the map editor instead: three consecutive frames, the outer
        // two mapped at adjacent virtual pages and the middle one left unmapped. The middle frame
        // is where a copy written as one memcpy over a translated base spills.
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
                    // The seam's own answer is folded into the bit: a copy that stopped at the
                    // first granule it could not acquire leaves a tail the byte compare below
                    // cannot always separate from a whole move.
                    bool const to_ok = kaccess_to_user(sa, cross, pat, sizeof(pat));
                    bool landed = to_ok;
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
                    bool const from_ok = kaccess_from_user(back, sa, cross, sizeof(back));
                    bool read_both = from_ok;
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
                    bool const ep_ok = ep_copy(sa, cross, sb, cross, 2 * HALF);
                    bool crossed = ep_ok;
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
                // Each release is the one that PAIRS: a release beside an acquire that
                // answered null surrenders a hold somebody else is holding, the count being of
                // calls (arch.h, arch_aspace_acquire).
                if (alo != nullptr)
                {
                    arch_aspace_release(sa, VA_A);
                }
                if (ahi != nullptr)
                {
                    arch_aspace_release(sa, VA_A + g);
                }
                if (blo != nullptr)
                {
                    arch_aspace_release(sb, VA_A);
                }
                if (bhi != nullptr)
                {
                    arch_aspace_release(sb, VA_A + g);
                }
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
            arch_aspace_release(space, VA_A);

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
            // Reached only where the load above did not fault: a backend whose unmap left the
            // translation standing answers a value here instead of a dump. Back to the caller's
            // own space, which is the root mapping the app text it returns to.
            aspace_activate_for(sched::current());
            arch_aspace_destroy(space);
            kickos_frame_free(frame);
            if (seen != PATTERN_B)
            {
                return 1; // the running translation never reached the frame
            }
            return 2 + static_cast<uint64_t>(after == PATTERN_B);
        }

        // The fourth transition as the unprivileged level sees it, which on a port that never
        // sets sstatus.SUM is the only level that can see it: a supervisor load of a page
        // carrying the unprivileged bit faults whether the leaf is still there or not.
        //
        // These two edit the CALLING task's own space and leave both loads to the caller. The
        // page is seeded here and the caller hands the word it read back, so the release below
        // runs only where the running translation reached this frame.
        constexpr uintptr_t VA_U = 0x20000000;
        arch_phys_addr_t g_here_frame = 0;

        struct arch_aspace* caller_space()
        {
            Thread const* const c = sched::current();
            if (c == nullptr)
            {
                return nullptr;
            }
            return domain_space(task_domain(c->task));
        }

        uint64_t op_map_here()
        {
            struct arch_aspace* const space = caller_space();
            if (space == nullptr or g_here_frame != 0)
            {
                return 0;
            }
            if (arch_aspace_frame_at(space, VA_U) != 0)
            {
                return 0; // the caller already maps it, so the read below would prove nothing
            }
            arch_phys_addr_t const frame = kickos_frame_alloc();
            if (frame == 0)
            {
                return 0;
            }
            if (arch_aspace_map(space, VA_U, frame, 1, ARCH_MAP_R | ARCH_MAP_W,
                                ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
            {
                kickos_frame_free(frame);
                return 0;
            }
            void* const seed = arch_aspace_acquire(space, VA_U);
            if (seed == nullptr)
            {
                (void)arch_aspace_unmap(space, VA_U, 1);
                kickos_frame_free(frame);
                return 0;
            }
            *word_at(seed) = PATTERN_B;
            arch_aspace_release(space, VA_U);
            g_here_frame = frame;
            return VA_U;
        }

        uint64_t op_unmap_here(uintptr_t seen)
        {
            struct arch_aspace* const space = caller_space();
            if (space == nullptr or g_here_frame == 0)
            {
                return 0;
            }
            if (static_cast<uint32_t>(seen) != PATTERN_B)
            {
                return 0;
            }
            // Announced before the unmap: the gate compares this address against the one the
            // fault report names, so the arm asserts WHICH page faulted.
            kprintf("[aspace] unmapped 0x%lx, expecting a translation fault\n",
                    static_cast<unsigned long>(VA_U));
            if (arch_aspace_unmap(space, VA_U, 1) != ARCH_ASPACE_OK)
            {
                return 0;
            }
            kickos_frame_free(g_here_frame);
            g_here_frame = 0;
            return 1;
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
        // The instrument is frame_pool_fail_in, and the sweep walks the refused attempt from the
        // first allocation a create makes to past its last. Measured immediately after each
        // refusal: a refusal that left the slot holding a half-built space balances anyway once
        // the next claim_slot releases it. The refusal counter carries the other half, a leaf left
        // standing over an already-returned frame being a second free when destroy walks the tree.
        //
        // `donor_base` at 0 sweeps the no-grant create. Anything else names a range the calling
        // task reserved and sweeps the grant-carrying create; the size is taken from the caller's
        // own list, so a number nobody reserved names no sweep.
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

        // A domain that is resolved and dropped must hand the root and its tables back. Two ways
        // it does not mask each other under one measurement, both freeing the same table: a
        // release that only decrements, and a free slot reused while its predecessor's space
        // still stands. Each is measured over a sequence the other cannot repair.
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
            // The release itself, weighed from after the resolve: over a resolve and a release
            // together the reuse cleanup above hands back exactly the space a missing release
            // destroy kept, and the pair reads as balanced.
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
            case KOS_ASPACE_OP_MAP_HERE:
            {
                return op_map_here();
            }
            case KOS_ASPACE_OP_UNMAP_HERE:
            {
                return op_unmap_here(a1);
            }
            case KOS_ASPACE_OP_SPLIT_ACCESS:
            {
                return op_split_access();
            }
            case KOS_ASPACE_OP_ACQUIRE_DUP:
            {
                return op_acquire_dup();
            }
            case KOS_ASPACE_OP_DOMAIN_BALANCE:
            {
                return op_domain_balance();
            }
            case KOS_ASPACE_OP_CAP_OBJECTS:
            {
                return op_cap_objects();
            }
            case KOS_ASPACE_OP_CAP_SEED:
            {
                return op_cap_seed();
            }
            case KOS_ASPACE_OP_CAP_SEED_VA:
            {
                return op_cap_seed_va();
            }
            case KOS_ASPACE_OP_CAP_SELF_SPACE:
            {
                return op_cap_self_space();
            }
            case KOS_ASPACE_OP_CAP_RUN_REFS:
            {
                return op_cap_run_refs();
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
                // Bit 0 says the posture is reached at all, bit 1 that the app half was written
                // in it. A guard that stops guarding lights bit 1; a posture that stopped
                // happening drops bit 0.
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
            case KOS_ASPACE_OP_ACTIVE_CORES:
            {
                uint32_t const held = domain_cores_on_held_space()
                                      | arch_aspace_active_cores(arch_aspace_boot());
                uint32_t mine = 0;
                Thread const* const c = sched::current();
                if (c != nullptr)
                {
                    mine = arch_aspace_active_cores(domain_space(task_domain(c->task)));
                }
                return (static_cast<uint64_t>(KICKOS_KERNEL_CORES) << 16)
                       | (static_cast<uint64_t>(popcount32(held)) << 8)
                       | static_cast<uint64_t>(popcount32(mine));
            }
            case KOS_ASPACE_OP_DOORBELL_COUNTS:
            {
                return arch_ipi_counts(static_cast<uint32_t>(a1));
            }
#if KICKOS_AMP_NODE
            case KOS_ASPACE_OP_AMP_ROUND:
            {
                return amp_round(static_cast<uint32_t>(a1));
            }
            case KOS_ASPACE_OP_AMP_COUNTS:
            {
                return amp_counts_packed(static_cast<uint32_t>(a1));
            }
            case KOS_ASPACE_OP_AMP_FORGE:
            {
                return amp_forge(static_cast<uint32_t>(a1));
            }
            case KOS_ASPACE_OP_AMP_RESETS:
            {
                return amp::counts(static_cast<uint32_t>(a1)).depth_reset.load();
            }
#endif
            case KOS_ASPACE_OP_SPACE_ID:
            {
                // Two tasks comparing this is what witnesses that a domain is an address
                // space of its own.
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
