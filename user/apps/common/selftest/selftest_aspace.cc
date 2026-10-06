// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The address-space arms: the map editor, frame capabilities, processes, and the stack as
// frames.

#include "selftest.h"

namespace selftest
{
#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    // The zero-authority worker joins main's task, so it shares these globals; only its authority
    // differs from main's.
    uintptr_t g_mint_seed = 0;
    uintptr_t g_mint_objects = 0;
    uintptr_t g_mint_self_space = 0;

    void unauthorised_mint_child(void*)
    {
        g_mint_seed = static_cast<uintptr_t>(kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0));
        g_mint_objects = static_cast<uintptr_t>(
            kos_aspace_probe(KOS_ASPACE_OP_CAP_OBJECTS, 0));
        g_mint_self_space = static_cast<uintptr_t>(
            kos_aspace_probe(KOS_ASPACE_OP_CAP_SELF_SPACE, 0));
        kos_sem_post(CH_DONE);
        kos_exit(0);
    }

    void t_cap_objects()
    {
        uintptr_t const b = kos_aspace_probe(KOS_ASPACE_OP_CAP_OBJECTS, 0);
        tap::diag("cap objects: bits 0x%x", static_cast<unsigned>(b));
        TAP_CHECK((b & KOS_ASPACE_CAPOBJ_FRAME_MINT) != 0);
        TAP_CHECK((b & KOS_ASPACE_CAPOBJ_FRAME_RESOLVE) != 0);
        TAP_CHECK((b & KOS_ASPACE_CAPOBJ_ASPACE_MINT) != 0);
        TAP_CHECK((b & KOS_ASPACE_CAPOBJ_ASPACE_HOLD) != 0);
        // A handle whose slot was reclaimed must not be answered by the next occupant.
        TAP_CHECK((b & KOS_ASPACE_CAPOBJ_ASPACE_STALE) != 0);
        TAP_CHECK((b & KOS_ASPACE_CAPOBJ_CLOSE_FRAMES) != 0);
        TAP_CHECK((b & KOS_ASPACE_CAPOBJ_CLOSE_HOLD) != 0);
        TAP_CHECK((b & KOS_ASPACE_CAPOBJ_BALANCED) != 0);
        // A frame handed back twice leaves the free count balanced and only this bit clear.
        TAP_CHECK((b & KOS_ASPACE_CAPOBJ_NO_REFUSED) != 0);

        g_mint_seed = 0;
        g_mint_objects = 0;
        g_mint_self_space = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        if (kos::thread::create_caps(unauthorised_mint_child, nullptr, "mintN", 10, caps, 1,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                     /*authority=*/0)
                .valid())
        {
            wait_n(1);
            uintptr_t const eperm = static_cast<uintptr_t>(-KOS_EPERM);
            tap::diag("unauthorised mint: seed %ld objects %ld self_space %ld",
                      static_cast<long>(g_mint_seed), static_cast<long>(g_mint_objects),
                      static_cast<long>(g_mint_self_space));
            TAP_CHECK(g_mint_seed == eperm);
            TAP_CHECK(g_mint_objects == eperm);
            // CAP_SELF_SPACE needs no allocation authority; its operations check permissions.
            TAP_CHECK(g_mint_self_space != eperm);
            TAP_CHECK(g_mint_self_space != 0);
        }
    }

    // The probe seeds the capabilities: no user-facing mint exists.
    void t_cap_map()
    {
        uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0);
        TAP_CHECK(seed != 0);
        if (seed == 0)
        {
            return;
        }
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFu);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        uintptr_t const va = static_cast<uintptr_t>(
            kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 0));
        TAP_CHECK(va != 0);

        // Permission comes from the AUTHORITY word, not a rights bit: the entry's rights field is
        // full.
        TAP_CHECK(kos_frame_map(fcap, acap, va, 0) == 0);
        volatile uint32_t* p = reinterpret_cast<volatile uint32_t*>(va);
        *p = 0xC2C2C2C2u;
        TAP_CHECK(*p == 0xC2C2C2C2u);

        TAP_CHECK(kos_frame_map(fcap, acap, va, 0) != 0);
        TAP_CHECK(kos_frame_map(fcap, acap, va + 1u, 0) != 0);
        TAP_CHECK(kos_frame_map(fcap, acap, va, 0xFFu) != 0);

        // A revoke matches the RUN and not a shape: a second run of the same length must not
        // revoke the first's mapping.
        uint64_t const seed2 = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0);
        // Asserted, never skipped: a skip here leaves the identity check untested.
        TAP_CHECK(seed2 != 0);
        kos_cap_t const other = static_cast<kos_cap_t>(seed2 & 0xFFFFFFFFu);
        TAP_CHECK(kos_frame_unmap(other, acap, va) != 0); // same length, different run
        kos_handle_close(other);
        kos_handle_close(static_cast<kos_cap_t>(seed2 >> 32));
        // An address inside a range and not its base is refused.
        uintptr_t const g2 = static_cast<uintptr_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        uintptr_t const inside_text = reinterpret_cast<uintptr_t>(&t_cap_map) & ~(g2 - 1u);
        TAP_CHECK(kos_frame_unmap(fcap, acap, inside_text) != 0);

        TAP_CHECK(kos_frame_unmap(fcap, acap, va) == 0);
        // Re-mapping is what says the unmap RELEASED the range. A second unmap refusing does
        // not: the backend refuses an already-unmapped range either way.
        TAP_CHECK(kos_frame_map(fcap, acap, va, 0) == 0);
        TAP_CHECK(kos_frame_unmap(fcap, acap, va) == 0);
        kos_handle_close(fcap);
        kos_handle_close(acap);
    }

    // Its task has a space of its own and NO reservation: the frame reaches it as a delegated
    // capability and nothing else.
    constexpr int CH_SHARE_FRAME = 2; // delegated SECOND, after CH_DONE
    constexpr uint32_t SHARE_A = 0xC3A11CE0u; // written by main, read by the child
    constexpr uint32_t SHARE_B = 0xC3B00B1Eu; // written by the child, read by main
    void share_child(void*)
    {
        uintptr_t const base = static_cast<uintptr_t>(
            kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 0));
        uintptr_t const g = static_cast<uintptr_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        // A DIFFERENT address from main's: the holder chooses.
        uintptr_t const va = base + g * 4u;
        kos_cap_t const space =
            static_cast<kos_cap_t>(kos_aspace_probe(KOS_ASPACE_OP_CAP_SELF_SPACE, 0));
        if (space != KOS_CAP_NONE and kos_frame_map(CH_SHARE_FRAME, space, va, 0) == 0)
        {
            volatile uint32_t* p = reinterpret_cast<volatile uint32_t*>(va);
            if (p[0] == SHARE_A)
            {
                p[1] = SHARE_B; // seen: answer through the same frame
            }
            // Reported through the FRAME and not a global: its task holds a space of its own.
            p[2] = static_cast<uint32_t>(va & 0xFFFFFFFFu);
            p[3] = static_cast<uint32_t>(static_cast<uint64_t>(va) >> 32);
        }
        kos_sem_post(CH_DONE);
    }

    void pin_child(void*)
    {
        uintptr_t const base = static_cast<uintptr_t>(
            kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 0));
        uintptr_t const g = static_cast<uintptr_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        kos_cap_t const space =
            static_cast<kos_cap_t>(kos_aspace_probe(KOS_ASPACE_OP_CAP_SELF_SPACE, 0));
        if (space != KOS_CAP_NONE)
        {
            (void)kos_frame_map(CH_SHARE_FRAME, space, base + g * 8u, 0);
        }
        kos_handle_close(CH_SHARE_FRAME); // the mapping is now this task's only hold
        kos_sem_post(CH_DONE);
    }

    // Read from the worker's own space to avoid capability chunks retained until reclaim.
    Atomic<uint32_t, Order::RELAXED> g_ssr_live{0};

    void ssr_worker(void*)
    {
        g_ssr_live = static_cast<uint32_t>(kos_aspace_probe(KOS_ASPACE_OP_RANGES_FREE, 0));
    }

    // A thread stack takes a range slot, so a slot leaked at thread exit exhausts the list.
    void t_stack_slot_returns()
    {
        uint64_t const free0 = kos_aspace_probe(KOS_ASPACE_OP_RANGES_FREE, 0);
        TAP_CHECK(free0 != 0);

        uint32_t live_low = static_cast<uint32_t>(free0);
        unsigned churned = 0;
        for (unsigned i = 0; i < 8u; i++)
        {
            g_ssr_live = 0;
            auto w = kos::thread::create(ssr_worker, nullptr, "ssr", 10);
            if (not w.valid())
            {
                break;
            }
            churned++;
            TAP_CHECK(w.join() == 0);
            uint32_t const live = g_ssr_live.load();
            if (live != 0 and live < live_low)
            {
                live_low = live;
            }
        }
        uint64_t const after = kos_aspace_probe(KOS_ASPACE_OP_RANGES_FREE, 0);
        tap::diag("stack slots: %u free, %u seen live over %u thread(s), %u after",
                  static_cast<unsigned>(free0), static_cast<unsigned>(live_low), churned,
                  static_cast<unsigned>(after));
        TAP_CHECK(churned > 0);
        // Positive control: a live stack takes a slot.
        TAP_CHECK(live_low < free0);
        TAP_CHECK(after == free0);
    }

    // Reject frame mappings over live stacks; replacement would overwrite thread locals.
    void t_cap_map_over_stack()
    {
        settle_exits();
        uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0);
        TAP_CHECK(seed != 0);
        if (seed == 0)
        {
            return;
        }
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFu);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        uintptr_t const g = static_cast<uintptr_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        TAP_CHECK(g != 0);

        // A local, so the page under it is this thread's own stack whatever the board's layout.
        volatile uint32_t canary = 0x5A17C0DEu;
        uintptr_t const page = reinterpret_cast<uintptr_t>(&canary) & ~(g - 1u);
        TAP_CHECK(kos_frame_map(fcap, acap, page, 0) != 0);
        TAP_CHECK(canary == 0x5A17C0DEu);
        // The reference the map takes BEFORE the record refuses it must come back, or a run
        // nobody mapped is held for good by a call that failed.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_CAP_RUN_REFS, 0) == 1u);

        // Control: a free address maps.
        uintptr_t const free_va =
            static_cast<uintptr_t>(kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 0));
        TAP_CHECK(free_va != 0);
        TAP_CHECK(kos_frame_map(fcap, acap, free_va, 0) == 0);
        TAP_CHECK(kos_frame_unmap(fcap, acap, free_va) == 0);
        kos_handle_close(fcap);
        kos_handle_close(acap);
    }

    // Image ranges, live stacks and their guards must fail caller-controlled admission.
    constexpr uint32_t SNP_BLK = 256;
    // Reuse one reservation: range slots cannot be freed through the user API.
    void* g_snp_blk = nullptr;

    void* snp_block()
    {
        if (g_snp_blk == nullptr)
        {
            void* const blk = kos_ram_alloc(SNP_BLK);
            if (blk != nullptr and kos_mem_self_grant(blk, SNP_BLK, 0) == 0)
            {
                g_snp_blk = blk;
            }
        }
        return g_snp_blk;
    }

    // Each caller scans before any grant attempt: a wrongly admitted grant could map the guard
    // and make a later scan find a different page.
    uintptr_t snp_guard(uintptr_t page, uintptr_t g)
    {
        uintptr_t p = page;
        for (unsigned i = 0; i < 64u and p >= g; i++)
        {
            uintptr_t const below = p - g;
            if (kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, below) == 0)
            {
                return below;
            }
            p = below;
        }
        return 0;
    }

    // Runs an arm's body on a stack the kernel placed with its guard below it, which main's own
    // stack is not: the init reserved it and handed it to main's task.
    enum class PoolStackBody
    {
        GRANT_REFUSED,
        GUARD_INTACT,
        HANDOFF_REFUSED
    };
    void stack_grant_refused_body();
    void stack_guard_intact_body();
    void stack_handoff_refused_body();
    void pool_stack_worker(void* arg) // caps: done@1
    {
        switch (static_cast<PoolStackBody>(reinterpret_cast<uintptr_t>(arg)))
        {
            case PoolStackBody::GRANT_REFUSED:
            {
                stack_grant_refused_body();
                break;
            }
            case PoolStackBody::GUARD_INTACT:
            {
                stack_guard_intact_body();
                break;
            }
            case PoolStackBody::HANDOFF_REFUSED:
            {
                stack_handoff_refused_body();
                break;
            }
        }
        kos_sem_post(CH_DONE);
    }
    void on_pool_stack(PoolStackBody body)
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(pool_stack_worker,
                                          reinterpret_cast<void*>(static_cast<uintptr_t>(body)),
                                          "pstk", 10, caps, 1, KOS_POLICY_FIFO, 0,
                                          /*privileged=*/false, nullptr, 0,
                                          KOS_AUTH_MEMORY | KOS_AUTH_TASKS);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        (void)w.join();
    }

    void stack_grant_refused_body()
    {
        uintptr_t const g = static_cast<uintptr_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        TAP_CHECK(g != 0);
        void* const mine = g_snp_blk;
        // A local, so the page under it is this thread's own stack whatever the board's layout.
        volatile uint32_t canary = 0x51AC0DE5u;
        uintptr_t const page = reinterpret_cast<uintptr_t>(&canary) & ~(g - 1u);
        uintptr_t const guard = snp_guard(page, g);
        TAP_CHECK(page != 0 and guard != 0);
        // Require a mapped page so rejection tests stack admission, not a missing range.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, page) != 0);

        // The guard address names the base of the entire stack range.
        TAP_CHECK(kos_mem_self_grant(reinterpret_cast<void*>(guard), g, 0) == -KOS_EPERM);
        TAP_CHECK(kos_mem_self_grant(reinterpret_cast<void*>(guard), 2u * g, 0) == -KOS_EPERM);
        // A different memory type reaches admission; plain R|W takes the already-accessible
        // shortcut.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 1) != 0);
        TAP_CHECK(kos_mem_self_grant(reinterpret_cast<void*>(page), g, KOS_MEM_NOCACHE)
                  == -KOS_EPERM);
        TAP_CHECK(canary == 0x51AC0DE5u);

        // Control: the same calls must accept a caller-owned reservation.
        TAP_CHECK(kos_mem_self_grant(mine, SNP_BLK, 0) == 0);
        TAP_CHECK(kos_mem_self_grant(mine, SNP_BLK, KOS_MEM_NOCACHE) == 0);
        TAP_CHECK(kos_mem_self_grant(mine, SNP_BLK, 0) == 0);
        // Still refused after a success on the same path, so it is not a one-shot state.
        TAP_CHECK(kos_mem_self_grant(reinterpret_cast<void*>(guard), g, 0) == -KOS_EPERM);
    }

    void t_stack_grant_refused()
    {
        if (snp_block() == nullptr)
        {
            tap::skip("no reservation left for the positive control");
            return;
        }
        on_pool_stack(PoolStackBody::GRANT_REFUSED);
    }

    // Check that the guard stays unmapped, independently of the grant return code.
    void stack_guard_intact_body()
    {
        uintptr_t const g = static_cast<uintptr_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        TAP_CHECK(g != 0);
        volatile uint32_t canary = 0x6BA2DEEDu;
        uintptr_t const page = reinterpret_cast<uintptr_t>(&canary) & ~(g - 1u);
        uintptr_t const guard = snp_guard(page, g);
        TAP_CHECK(guard != 0);
        tap::diag("stack guard at 0x%lx, one granule below the run's low bound",
                  static_cast<unsigned long>(guard));
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, guard) == 0);

        // Every way a caller can name the run, each followed by the guard's translation.
        (void)kos_mem_self_grant(reinterpret_cast<void*>(guard), g, 0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, guard) == 0);
        (void)kos_mem_self_grant(reinterpret_cast<void*>(guard), 2u * g, 0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, guard) == 0);
        (void)kos_mem_self_grant(reinterpret_cast<void*>(page), g, KOS_MEM_NOCACHE);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, guard) == 0);
        TAP_CHECK(canary == 0x6BA2DEEDu);
    }

    void t_stack_guard_intact()
    {
        on_pool_stack(PoolStackBody::GUARD_INTACT);
    }

    // Reject donation of the live stack: its frames are freed when the donor exits.
    void stack_handoff_refused_body()
    {
        uintptr_t const g = static_cast<uintptr_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        TAP_CHECK(g != 0);
        void* const mine = g_snp_blk;
        volatile uint32_t canary = 0x4A0FF5EDu;
        uintptr_t const page = reinterpret_cast<uintptr_t>(&canary) & ~(g - 1u);
        uintptr_t const guard = snp_guard(page, g);
        TAP_CHECK(guard != 0);
        uint64_t const frames = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);

        // Every extent: the app does not know the board's root-stack size, and handoff requires an
        // exact base and page count.
        unsigned admitted = 0;
        int32_t last = 0;
        for (unsigned pages = 1; pages <= 32u; pages++)
        {
            kos_task_t t = KOS_TASK_NONE;
            last = kos_task_create(reinterpret_cast<void*>(guard),
                                   static_cast<size_t>(pages) * g, 0, &t);
            if (last == 0)
            {
                admitted++;
                (void)kos_task_kill(t);
            }
        }
        tap::diag("stack handoff: %u of 32 extents admitted, last code %d", admitted,
                  static_cast<int>(last));
        TAP_CHECK(admitted == 0);
        TAP_CHECK(canary == 0x4A0FF5EDu);
        // A refused handoff frees the space it half-built, so no frame is spent by the sweep.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0) == frames);

        // Control: accept handoff of a caller-owned reservation.
        kos_task_t ok = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(mine, SNP_BLK, 0, &ok) == 0);
        TAP_CHECK(kos_task_kill(ok) == 0);
    }

    void t_stack_handoff_refused()
    {
        settle_exits();
        if (snp_block() == nullptr)
        {
            tap::skip("no reservation left for the positive control");
            return;
        }
        on_pool_stack(PoolStackBody::HANDOFF_REFUSED);
    }

    uintptr_t g_live_rest = 0;
    constexpr uint64_t SETTLE_POLL_NS = 100000ull;

    // A thread returns its stack frames, and as its task's last member its space, inside its
    // own exit, which runs after the done post main waits on and after any kill returns. Read a
    // frame or space count only after this, and call it only where the arm holds no thread
    // alive, or it waits for that one too.
    void settle_exits()
    {
        while (kos_aspace_probe(KOS_ASPACE_OP_THREADS_LIVE, 0) > g_live_rest)
        {
            kos_sleep_ns(SETTLE_POLL_NS);
        }
    }

    // A MAPPING IS A HOLDER. Without that the last capability's drop frees frames a live leaf
    // still points at, and reading the page does not say so.
    void t_cap_map_pins_run()
    {
        settle_exits();
        uint64_t const free0 = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        uintptr_t const spaces0 = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
        uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0);
        if (seed == 0)
        {
            tap::skip("no frame run to pin");
            return;
        }
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFu);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0) == free0 - 1u);
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            kos_handle_close(fcap);
            kos_handle_close(acap);
            tap::skip("task pool too small");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {fcap, KOS_CAP_TRANSFER}};
        if (not kos::thread::create_caps(pin_child, nullptr, "pin", 10, caps, 2,
                                         KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                         KOS_AUTH_MEMORY, nullptr, t)
                    .valid())
        {
            (void)kos_task_kill(t);
            kos_handle_close(fcap);
            kos_handle_close(acap);
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        // main's capability plus the child's MAPPING; the child dropped its own.
        uint64_t const refs_after_child = kos_aspace_probe(KOS_ASPACE_OP_CAP_RUN_REFS, 0);
        tap::diag("cap pin: refs after the child mapped and closed = %u",
                  static_cast<unsigned>(refs_after_child));
        TAP_CHECK(refs_after_child == 2u);
        // main drops its capability too: from here NO capability names the run.
        kos_handle_close(fcap);
        kos_handle_close(acap);
        // Only the mapping holds the run. The child's own space also consumes frames.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_CAP_RUN_REFS, 0) == 1u);
        (void)kos_task_kill(t);
        settle_exits();
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0) == spaces0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_CAP_RUN_REFS, 0) == 0u);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0) == free0);
    }

    void t_cap_share()
    {
        settle_exits();
        uint64_t const free0 = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        uintptr_t const spaces0 = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
        uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0);
        if (seed == 0)
        {
            tap::skip("no frame run to share");
            return;
        }
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFu);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        uintptr_t const va = static_cast<uintptr_t>(
            kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 0));
        TAP_CHECK(kos_frame_map(fcap, acap, va, 0) == 0);
        volatile uint32_t* const mine = reinterpret_cast<volatile uint32_t*>(va);
        mine[0] = SHARE_A;
        mine[1] = 0;
        mine[2] = 0;
        mine[3] = 0;

        // A task of its OWN, carrying no grant: a null mem_base skips the handoff.
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            tap::skip("task pool too small");
            return;
        }
        // Only TRANSFER is valid; neither capability type carries access rights.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {fcap, KOS_CAP_TRANSFER}};
        // The negative control below omits this authority.
        if (not kos::thread::create_caps(share_child, nullptr, "shr", 10, caps, 2,
                                         KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                         KOS_AUTH_MEMORY, nullptr, t)
                    .valid())
        {
            (void)kos_task_kill(t);
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        uintptr_t const theirs = static_cast<uintptr_t>(mine[2])
                                 | (static_cast<uintptr_t>(mine[3]) << 32);
        tap::diag("cap share: main 0x%x, peer 0x%x",
                  static_cast<unsigned>(va), static_cast<unsigned>(theirs));
        TAP_CHECK(theirs != 0);
        TAP_CHECK(theirs != va);
        TAP_CHECK(mine[1] == SHARE_B);

        // The borrower dies while main still maps the run: the run belongs to the CAPABILITY,
        // so the space that mapped it owned nothing to free.
        (void)kos_task_kill(t);
        settle_exits();
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0) == spaces0);
        // Only the pool count sees an early free; a stale mapping still reads back.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0) == free0 - 1u);
        mine[0] = SHARE_A + 1u;
        TAP_CHECK(mine[0] == SHARE_A + 1u);

        // An identical child with NO authority cannot map the same delegated capability:
        // possession of the frame is not permission to map it.
        mine[2] = 0;
        mine[3] = 0;
        kos_task_t t2 = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t2) == 0)
        {
            if (kos::thread::create_caps(share_child, nullptr, "shrN", 10, caps, 2,
                                         KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                         /*authority=*/0, nullptr, t2)
                    .valid())
            {
                wait_n(1);
                TAP_CHECK(mine[2] == 0); // it reached the map and was refused
            }
            (void)kos_task_kill(t2);
            settle_exits();
            TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0) == spaces0);
        }

        TAP_CHECK(kos_frame_unmap(fcap, acap, va) == 0);
        kos_handle_close(fcap);
        kos_handle_close(acap);
        // The frames come back only once the LAST capability naming the run is gone.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_CAP_RUN_REFS, 0) == 0u);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0) == free0);
    }

    void t_aspace_seam()
    {
        settle_exits();
        uintptr_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
        TAP_CHECK(g != 0);
        TAP_CHECK((g & (g - 1u)) == 0);
        // All three types honoured, so no grant is admitted and then quietly downgraded.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 0) == 1); // normal
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 1) == 1); // non-cacheable
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 2) == 1); // device
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 3) == 0); // no such type
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0) != 0);
        tap::diag("aspace: granule %u, %u frames free",
                  static_cast<unsigned>(g),
                  static_cast<unsigned>(kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0)));
    }

    // Do not change architectural limits to match an emulator.
    void t_aspace_model()
    {
        uint64_t const m = kos_aspace_probe(KOS_ASPACE_OP_MODEL, 0);
        unsigned const asid =
            static_cast<unsigned>((m >> KOS_ASPACE_MODEL_ASID_SHIFT) & KOS_ASPACE_MODEL_FIELD_MASK);
        unsigned const pa =
            static_cast<unsigned>((m >> KOS_ASPACE_MODEL_PA_SHIFT) & KOS_ASPACE_MODEL_FIELD_MASK);
        unsigned const grans =
            static_cast<unsigned>((m >> KOS_ASPACE_MODEL_GRAN_SHIFT) & KOS_ASPACE_MODEL_FIELD_MASK);
        tap::diag("aspace model: granules 0x%x, %u ASID bits, %u PA bits, verdict 0x%x",
                  grans, asid, pa, static_cast<unsigned>(m & KOS_ASPACE_MODEL_FIELD_MASK));
        // A nonzero physical width excludes an empty report. ASID width may legally be zero on an
        // untagged backend.
        TAP_CHECK(pa != 0 and grans != 0);
        TAP_CHECK((m & KOS_ASPACE_MODEL_ALL) == KOS_ASPACE_MODEL_ALL);
        TAP_CHECK((m & KOS_ASPACE_MODEL_TAGGED) == 0 or asid != 0);
    }

    void t_aspace_map_cycle()
    {
        // Map, read/write and unmap, including absence of the final translation.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_ROUNDTRIP, 0) == KOS_ASPACE_TRIP_GONE);
    }

    void t_aspace_translate()
    {
        // Two virtual pages on one frame, written through both aliases.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_ALIAS, 0) == 1);
    }

    void t_aspace_refusals()
    {
        // Invalid addresses, alignment, size, rights, partial map/unmap ranges and a run crossing
        // the reported physical-address limit.
        uintptr_t const bits = kos_aspace_probe(KOS_ASPACE_OP_REFUSALS, 0);
        tap::diag("map editor refusal word 0x%x", static_cast<unsigned>(bits));
        TAP_CHECK(bits == KOS_ASPACE_REFUSE_ALL);
    }

    void t_aspace_span()
    {
        // Crosses two leaf-table boundaries; the length derives from the granule.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_SPAN, 0) == 1);
    }

    void t_aspace_acquire_dup()
    {
        // Acquire one page twice, release once: the surviving hold keeps its window slot from
        // reuse for another page.
        uintptr_t const bits = kos_aspace_probe(KOS_ASPACE_OP_ACQUIRE_DUP, 0);
        tap::diag("acquire duplicate-hold word 0x%x", static_cast<unsigned>(bits));
        TAP_CHECK(bits == KOS_ASPACE_DUP_ALL);
    }

    void t_aspace_balance()
    {
        settle_exits();
        // Repeated create/map/unmap/destroy returns data and table frames.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    void t_aspace_domain_balance()
    {
        settle_exits();
        // Dropping a domain must release its space before the slot is reused.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_DOMAIN_BALANCE, 0) == 0);
    }

    // Fails each create allocation in turn. The injection depth is checked as well as the bits,
    // or an empty sweep passes.
    void t_aspace_forced_unwind()
    {
        uintptr_t const r = kos_aspace_probe(KOS_ASPACE_OP_FORCED_UNWIND, 0);
        uintptr_t const bits = r & KOS_ASPACE_UNWIND_ALL;
        uintptr_t const depth = r >> KOS_ASPACE_UNWIND_DEPTH_SHIFT;
        tap::diag("forced unwind: bits %u of %u over %u injected allocation(s)",
                  static_cast<unsigned>(bits),
                  static_cast<unsigned>(KOS_ASPACE_UNWIND_ALL),
                  static_cast<unsigned>(depth));
        TAP_CHECK(depth >= KOS_ASPACE_UNWIND_MIN_DEPTH);
        TAP_CHECK(bits == KOS_ASPACE_UNWIND_ALL);
        // With a grant: borrower cleanup must unmap without freeing donor frames. This layout
        // reuses an image leaf table, so failures reach claim_slot but not domain_for.
        void* const block = kos_ram_alloc(64);
        if (block == nullptr)
        {
            tap::skip("no reservation left to sweep the grant-carrying create with");
            return;
        }
        uintptr_t const h = kos_aspace_probe(KOS_ASPACE_OP_FORCED_UNWIND,
                                            reinterpret_cast<uintptr_t>(block));
        uintptr_t const hbits = h & KOS_ASPACE_UNWIND_ALL;
        uintptr_t const hdepth = h >> KOS_ASPACE_UNWIND_DEPTH_SHIFT;
        tap::diag("forced unwind, with a grant: bits %u of %u over %u injected allocation(s)",
                  static_cast<unsigned>(hbits),
                  static_cast<unsigned>(KOS_ASPACE_UNWIND_ALL),
                  static_cast<unsigned>(hdepth));
        TAP_CHECK(hdepth >= KOS_ASPACE_UNWIND_MIN_DEPTH);
        TAP_CHECK(hbits == KOS_ASPACE_UNWIND_ALL);
    }

    // One pool balance across normal task teardown and failed creation, interleaved.
    // Minimum expected cost: one root, two image tables and one private data page.
    constexpr uintptr_t CHURN_MIN_FRAMES = 4;

    void churn_member(void*)
    {
        kos_exit(0);
    }

    // `held` is read with the space ALIVE and before the member starts, so it is the space's own
    // cost and races with nothing.
    bool churn_cycle(uintptr_t* held)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            return false;
        }
        if (held != nullptr)
        {
            *held = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        }
        auto const m = kos::thread::create_caps(churn_member, nullptr, "chrn", 10, nullptr, 0,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                               nullptr, t);
        if (not m.valid())
        {
            (void)kos_task_kill(t);
            return false;
        }
        bool const joined = m.join() == 0;
        // Drop the creator hold before checking reclamation; thread exit leaves it live.
        bool const reaped = kos_task_kill(t) == 0;
        return joined and reaped;
    }

    void t_aspace_churn()
    {
        settle_exits();
        // Warm up first: initial mappings allocate intermediate tables retained for reuse.
        if (not churn_cycle(nullptr))
        {
            tap::skip("no task or thread slot to churn with");
            return;
        }
        uintptr_t const frames0 = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        uintptr_t const roots0 = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
        uintptr_t low = frames0;
        for (int i = 0; i < 4; i++)
        {
            uintptr_t held = frames0;
            TAP_CHECK(churn_cycle(&held));
            if (held < low)
            {
                low = held;
            }
            uintptr_t const u = kos_aspace_probe(KOS_ASPACE_OP_FORCED_UNWIND, 0);
            TAP_CHECK((u & KOS_ASPACE_UNWIND_ALL) == KOS_ASPACE_UNWIND_ALL);
            TAP_CHECK((u >> KOS_ASPACE_UNWIND_DEPTH_SHIFT) >= KOS_ASPACE_UNWIND_MIN_DEPTH);
        }
        uintptr_t const frames1 = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        uintptr_t const roots1 = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
        tap::diag("churn: %u frames free, %u with a process live, %u after; roots %u then %u",
                  static_cast<unsigned>(frames0), static_cast<unsigned>(low),
                  static_cast<unsigned>(frames1), static_cast<unsigned>(roots0),
                  static_cast<unsigned>(roots1));
        // Require a live-process allocation to exclude a trivially balanced empty cycle.
        TAP_CHECK(low + CHURN_MIN_FRAMES <= frames0);
        TAP_CHECK(frames1 == frames0);
        TAP_CHECK(roots1 == roots0);
        // BALANCE folds in the refusal counter: no path above handed the pool a frame twice.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // Warm up first: intermediate tables are retained after the first mapping.
    kos_cap_t g_sfgate = KOS_CAP_NONE;
    void sframe_worker(void*) // caps: done@1, gate@2
    {
        kos_sem_post(CH_DONE);
        kos_sem_wait(2); // held alive while main reads the pool
    }
    bool sframe_cycle(uintptr_t* live)
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_sfgate, CH_FULL}};
        auto w = kos::thread::create_caps(sframe_worker, nullptr, "sfram", 10, caps, 2);
        if (not w.valid())
        {
            return false;
        }
        wait_n(1);
        if (live != nullptr)
        {
            *live = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        }
        kos_sem_post(g_sfgate);
        return w.join() == 0;
    }
    void t_stack_is_frames()
    {
        settle_exits();
        TAP_CHECK(kos_sem_create(0, &g_sfgate) == 0);
        TAP_CHECK(sframe_cycle(nullptr));
        uintptr_t const before = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        uintptr_t live = 0;
        TAP_CHECK(sframe_cycle(&live));
        // At least the stack and the unmapped page below it.
        TAP_CHECK(before >= live + 2);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0) == before);
        tap::diag("stack frames: %u held by one live thread",
                  static_cast<unsigned>(before - live));
        // BALANCE includes the cumulative refused-free count, so it covers stack reclamation.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
        TAP_CHECK(kos_handle_close(g_sfgate) == 0);
        g_sfgate = KOS_CAP_NONE;
    }

    // Both workers are created before either runs, so their domains coexist. Each reports
    // through a shared grant; process-private globals cannot carry results.
    void space_id_worker(void* arg)
    {
        uint32_t const id = static_cast<uint32_t>(kos_aspace_probe(KOS_ASPACE_OP_SPACE_ID, 0));
        *static_cast<volatile uint32_t*>(arg) = id;
        kos_sem_post(CH_DONE);
    }

    // Waits for any worker that started, even if the second fails, so its g_done post cannot be
    // consumed by a later test.
    bool two_space_ids(void* mem_base, uint32_t mem_size, volatile uint32_t* slot)
    {
        slot[0] = 0;
        slot[1] = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        int seated = 0;
        if (kos::thread::create_caps(space_id_worker, const_cast<uint32_t*>(&slot[0]), "spidA",
                                     10, caps, 1, KOS_POLICY_FIFO, 0, false, mem_base,
                                     mem_size).valid())
        {
            seated++;
        }
        if (kos::thread::create_caps(space_id_worker, const_cast<uint32_t*>(&slot[1]), "spidB",
                                     10, caps, 1, KOS_POLICY_FIFO, 0, false, mem_base,
                                     mem_size).valid())
        {
            seated++;
        }
        wait_n(seated);
        return seated == 2;
    }

    void t_aspace_two_spaces_same_grant()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        void* const shared = kos_ram_alloc(256);
        if (shared == nullptr)
        {
            tap::skip("arena cannot spare the shared region");
            return;
        }
        // main maps the block too: the pair answers through it.
        TAP_CHECK(kos_mem_self_grant(shared, 256, 0) == 0);
        volatile uint32_t* const slot = static_cast<volatile uint32_t*>(shared);
        if (not two_space_ids(shared, 256, slot))
        {
            tap::skip("thread pool too small for 2 concurrent");
            return;
        }
        uint32_t const ida = slot[0];
        uint32_t const idb = slot[1];
        TAP_CHECK(ida != 0 and idb != 0);
        TAP_CHECK(ida != idb);
    }

    // Without grants, report over an endpoint; each process owns separate globals.
    void space_id_ep_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        uint32_t const id = static_cast<uint32_t>(kos_aspace_probe(KOS_ASPACE_OP_SPACE_ID, 0));
        (void)kos_send(2, &id, sizeof(id));
        kos_sem_post(CH_DONE);
    }

    // Plain spawns would share main's task, so both tasks are created explicitly.
    bool two_space_ids_own_tasks(uint32_t* ida, uint32_t* idb)
    {
        *ida = 0;
        *idb = 0;
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            return false;
        }
        kos_task_t ta = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &ta) != 0)
        {
            (void)kos_handle_close(ep);
            return false;
        }
        if (kos_task_create(nullptr, 0, 0, &tb) != 0)
        {
            (void)kos_task_kill(ta);
            (void)kos_handle_close(ep);
            return false;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_SIGNAL}};
        int seated = 0;
        if (kos::thread::create_caps(space_id_ep_worker, nullptr, "spidA", 10, caps, 2,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                     ta).valid())
        {
            seated++;
        }
        if (kos::thread::create_caps(space_id_ep_worker, nullptr, "spidB", 10, caps, 2,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                     tb).valid())
        {
            seated++;
        }
        // Receive before waiting for completion: workers post only after send returns.
        for (int i = 0; i < seated; i++)
        {
            uint32_t id = 0;
            struct kos_reply_recv_opts idopts;
            kos_reply_recv_opts_init(&idopts, ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
            if (kos_reply_recv(KOS_CAP_NONE, &id, kos_call_lens_pack(0, sizeof(id)), &idopts)
                != static_cast<int32_t>(sizeof(id)))
            {
                break;
            }
            if (i == 0)
            {
                *ida = id;
            }
            else
            {
                *idb = id;
            }
        }
        wait_n(seated);
        // After the members are gone, so each kill only drops main's hold on an empty group.
        (void)kos_task_kill(ta);
        (void)kos_task_kill(tb);
        (void)kos_handle_close(ep);
        return seated == 2;
    }

    void t_aspace_two_spaces_no_grant()
    {
        // Two tasks with nothing granted must still be two address spaces.
        uint32_t ida = 0;
        uint32_t idb = 0;
        if (not two_space_ids_own_tasks(&ida, &idb))
        {
            tap::skip("thread, task or endpoint pool too small for 2 concurrent");
            return;
        }
        TAP_CHECK(ida != 0 and idb != 0);
        TAP_CHECK(ida != idb);
    }

    // One virtual address, private data frames, shared text frames. Each worker reports through
    // its own block; private globals cannot report to main.
    enum
    {
        PW_ADDR = 0,     // the member's own &g_pw_word
        PW_DATA_FRAME = 1,
        PW_TEXT_FRAME = 2,
        PW_READBACK = 3,
        PW_SEED = 4, // main's, read by the member: the value only that member writes
        PW_WORDS = 5
    };
    volatile uint32_t g_pw_word = 0u;
    void pw_member(void* arg) // caps: done@1, gate@2
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        uint32_t const mine = static_cast<uint32_t>(out[PW_SEED]);
        out[PW_ADDR] = reinterpret_cast<uintptr_t>(&g_pw_word);
        out[PW_DATA_FRAME] =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(&g_pw_word));
        out[PW_TEXT_FRAME] =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(&pw_member));
        g_pw_word = mine;
        kos_sem_post(CH_DONE);
        kos_sem_wait(CH_LOCK); // the gate, delegated second; both members have written by now
        out[PW_READBACK] = g_pw_word;
        kos_sem_post(CH_DONE);
    }
    void t_process_private_data()
    {
        constexpr uint32_t PW_BLK = 256;
        constexpr uint32_t PW_A = 0xA5A50F0Fu;
        constexpr uint32_t PW_B = 0x5A5AF0F0u;
        void* const ba = kos_ram_alloc(PW_BLK);
        void* const bb = kos_ram_alloc(PW_BLK);
        if (ba == nullptr or bb == nullptr)
        {
            tap::skip("arena cannot spare two report blocks");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(ba, PW_BLK, 0) == 0);
        TAP_CHECK(kos_mem_self_grant(bb, PW_BLK, 0) == 0);
        volatile uint64_t* const oa = static_cast<volatile uint64_t*>(ba);
        volatile uint64_t* const ob = static_cast<volatile uint64_t*>(bb);
        for (int i = 0; i < PW_WORDS; i++)
        {
            oa[i] = 0;
            ob[i] = 0;
        }
        oa[PW_SEED] = PW_A;
        ob[PW_SEED] = PW_B;
        g_pw_word = 0x600Du;
        kos_cap_t gate = KOS_CAP_NONE;
        if (kos_sem_create(0, &gate) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        kos_task_t ta = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        if (kos_task_create(ba, PW_BLK, 0, &ta) != 0 or kos_task_create(bb, PW_BLK, 0, &tb) != 0)
        {
            (void)kos_task_kill(ta);
            (void)kos_handle_close(gate);
            tap::skip("task pool too small for 2 groups");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {gate, CH_FULL}};
        int seated = 0;
        if (kos::thread::create_caps(pw_member, ba, "pwA", 10, caps, 2, KOS_POLICY_FIFO, 0,
                                     false, nullptr, 0, 0, nullptr, ta).valid())
        {
            seated++;
        }
        if (kos::thread::create_caps(pw_member, bb, "pwB", 10, caps, 2, KOS_POLICY_FIFO, 0,
                                     false, nullptr, 0, 0, nullptr, tb).valid())
        {
            seated++;
        }
        if (seated != 2)
        {
            // Release the worker that started so its completion stays in this test.
            wait_n(seated);
            kos_sem_post(gate);
            wait_n(seated);
            (void)kos_task_kill(ta);
            (void)kos_task_kill(tb);
            (void)kos_handle_close(gate);
            tap::skip("thread pool too small for 2 concurrent");
            return;
        }
        wait_n(2); // both have written their own copy
        kos_sem_post(gate);
        kos_sem_post(gate);
        wait_n(2); // both have read it back
        (void)kos_task_kill(ta);
        (void)kos_task_kill(tb);
        (void)kos_handle_close(gate);
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pw_word);
        uint64_t const rootdf =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, own);
        uint64_t const roottf =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(&pw_member));
        tap::diag("process: data frames %u/%u/%u, text frame %u",
                  static_cast<unsigned>(rootdf), static_cast<unsigned>(oa[PW_DATA_FRAME]),
                  static_cast<unsigned>(ob[PW_DATA_FRAME]), static_cast<unsigned>(roottf));
        TAP_CHECK(oa[PW_ADDR] == own and ob[PW_ADDR] == own);
        TAP_CHECK(rootdf != 0 and oa[PW_DATA_FRAME] != 0 and ob[PW_DATA_FRAME] != 0);
        TAP_CHECK(oa[PW_DATA_FRAME] != ob[PW_DATA_FRAME]
                  and oa[PW_DATA_FRAME] != rootdf and ob[PW_DATA_FRAME] != rootdf);
        // ONE frame under the text: the control the copy is measured against.
        TAP_CHECK(roottf != 0 and oa[PW_TEXT_FRAME] == roottf and ob[PW_TEXT_FRAME] == roottf);
        // Each read back its OWN write, after the other had written the same address.
        TAP_CHECK(oa[PW_READBACK] == PW_A and ob[PW_READBACK] == PW_B);
        TAP_CHECK(g_pw_word == 0x600Du); // and main's copy was untouched by either
    }

    // Two task siblings share one image. The writer, the task's entry, holds the task open
    // until the reader has read: its exit ends the task.
    volatile uint32_t g_sib_word = 0u;
    void sib_writer(void* arg) // caps: done@1, hold@2
    {
        g_sib_word = 0xBEEFu;
        static_cast<volatile uint64_t*>(arg)[0] = 1u;
        kos_sem_post(CH_DONE);
        kos_sem_wait(2);
    }
    void sib_reader(void* arg) // caps: done@1
    {
        static_cast<volatile uint64_t*>(arg)[1] = g_sib_word;
        kos_sem_post(CH_DONE);
    }
    void t_task_siblings_share()
    {
        constexpr uint32_t SIB_BLK = 256;
        void* const blk = kos_ram_alloc(SIB_BLK);
        if (blk == nullptr)
        {
            tap::skip("arena cannot spare a report block");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(blk, SIB_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        out[0] = 0;
        out[1] = 0;
        g_sib_word = 0u;
        kos_task_t t = KOS_TASK_NONE;
        kos_cap_t hold = KOS_CAP_NONE;
        if (kos_task_create(blk, SIB_BLK, 0, &t) != 0 or kos_sem_create(0, &hold) != 0)
        {
            tap::skip("task or semaphore pool too small");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {hold, KOS_CAP_WAIT}};
        // Wait for the writer before starting the reader; spawn alone does not preempt.
        if (not kos::thread::create_caps(sib_writer, blk, "sibW", 10, caps, 2, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, 0, nullptr, t).valid())
        {
            (void)kos_task_kill(t);
            (void)kos_handle_close(hold);
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        bool const read = kos::thread::create_caps(sib_reader, blk, "sibR", 10, caps, 1,
                                                   KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                                   nullptr, t)
                              .valid();
        if (read)
        {
            wait_n(1);
        }
        kos_sem_post(hold);
        (void)kos_task_kill(t);
        (void)kos_handle_close(hold);
        TAP_CHECK(read);
        TAP_CHECK(out[0] == 1u);          // the writer ran
        TAP_CHECK(out[1] == 0xBEEFu);     // and its sibling saw the store: one image, one group
        TAP_CHECK(g_sib_word == 0u);      // main did not: a different group is a different copy
    }

    // Handoff at the same address, through both task creation and a grant-carrying spawn.
    enum
    {
        HO_SEEN = 0,
        HO_ADDR = 1,
        HO_FRAME = 2,
        HO_TYPE = 3, // 1 + the memory type this space's mapping of the block carries
        HO_WORDS = 4
    };
    constexpr uint64_t HO_SENTINEL = 0x0D15EA5Eu;
    void ho_reader(void* arg) // caps: done@1
    {
        volatile uint64_t* const blk = static_cast<volatile uint64_t*>(arg);
        uint64_t const seen = blk[HO_SEEN];
        blk[HO_ADDR] = reinterpret_cast<uintptr_t>(arg);
        // One frame under two spaces, which a target given frames of its own would answer
        // differently.
        blk[HO_FRAME] = kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(arg));
        blk[HO_TYPE] =
            kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, reinterpret_cast<uintptr_t>(arg));
        blk[HO_SEEN] = seen + 1u; // the readback, echoed back through the same bytes
        kos_sem_post(CH_DONE);
    }
    void t_task_handoff_readback()
    {
        settle_exits();
        constexpr uint32_t HO_BLK = 256;
        void* const blk = kos_ram_alloc(HO_BLK);
        if (blk == nullptr)
        {
            tap::skip("arena cannot spare the shared block");
            return;
        }
        // Reach it before writing it: allocation grants nothing.
        TAP_CHECK(kos_mem_self_grant(blk, HO_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        out[HO_SEEN] = HO_SENTINEL;
        out[HO_ADDR] = 0;
        out[HO_FRAME] = 0;
        uint64_t const mine =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(blk));
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(blk, HO_BLK, 0, &t) != 0)
        {
            tap::skip("task pool too small");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        // Path one: an explicit task create.
        if (not kos::thread::create_caps(ho_reader, blk, "hoT", 10, caps, 1, KOS_POLICY_FIFO, 0,
                                         false, nullptr, 0, 0, nullptr, t).valid())
        {
            (void)kos_task_kill(t);
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        (void)kos_task_kill(t);
        uint64_t const first_seen = out[HO_SEEN];
        uint64_t const first_addr = out[HO_ADDR];
        uint64_t const first_frame = out[HO_FRAME];
        out[HO_SEEN] = HO_SENTINEL;
        out[HO_ADDR] = 0;
        out[HO_FRAME] = 0;
        // Path two: the grant-carrying spawn, same range.
        if (not kos::thread::create_caps(ho_reader, blk, "hoS", 10, caps, 1, KOS_POLICY_FIFO, 0,
                                         false, blk, HO_BLK).valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        TAP_CHECK(mine != 0 and first_frame == mine and out[HO_FRAME] == mine);
        TAP_CHECK(first_seen == HO_SENTINEL + 1u);   // the child read what main wrote
        TAP_CHECK(first_addr == reinterpret_cast<uintptr_t>(blk)); // at the address main named
        TAP_CHECK(out[HO_SEEN] == HO_SENTINEL + 1u);
        TAP_CHECK(out[HO_ADDR] == reinterpret_cast<uintptr_t>(blk));
        // A nondefault type on a separate block: all aliases of one block carry one type. Task
        // creation only; the spawn grant ABI has no memory-type field and passes zero.
        void* const typed = kos_ram_alloc(HO_BLK);
        if (typed == nullptr)
        {
            tap::skip("no reservation left for the typed handoff");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(typed, HO_BLK, KOS_MEM_NOCACHE) == 0);
        volatile uint64_t* const tout = static_cast<volatile uint64_t*>(typed);
        tout[HO_SEEN] = HO_SENTINEL;
        tout[HO_ADDR] = 0;
        tout[HO_TYPE] = 0;
        // 1 + ARCH_MAP_NOCACHE, the non-cacheable type KOS_ASPACE_OP_MEMTYPE numbers 1.
        constexpr uint64_t HO_NOCACHE_AT = 2u;
        uint64_t const donor_type =
            kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, reinterpret_cast<uintptr_t>(typed));
        kos_task_t tt = KOS_TASK_NONE;
        if (kos_task_create(typed, HO_BLK, KOS_MEM_NOCACHE, &tt) != 0)
        {
            tap::skip("task pool too small for the typed handoff");
            return;
        }
        if (not kos::thread::create_caps(ho_reader, typed, "hoN", 10, caps, 1, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, 0, nullptr, tt).valid())
        {
            (void)kos_task_kill(tt);
            tap::skip("thread pool too small for the typed handoff");
            return;
        }
        wait_n(1);
        (void)kos_task_kill(tt);
        settle_exits();
        TAP_CHECK(donor_type == HO_NOCACHE_AT);
        TAP_CHECK(tout[HO_TYPE] == donor_type); // the two live mappings agree
        TAP_CHECK(tout[HO_SEEN] == HO_SENTINEL + 1u);
        TAP_CHECK(tout[HO_ADDR] == reinterpret_cast<uintptr_t>(typed));
        // Borrowers exited while main still maps the block: no donor frame may have been freed.
        // BALANCE reports refused frees as all ones.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // An interior base in a two-page reservation is refused: accepting the whole reservation
    // would expose a donor page the caller did not name.
    enum
    {
        SL_GRAN = 0, // the granule, so the borrower can name the page below its own
        SL_ECHO = 1  // where the borrower reports what it read there
    };
    constexpr uint64_t SL_TOKEN = 0x5A17ED10u;

    // Reached only where the refusal did not hold: reads the page below the one handed over
    // and echoes it into the page that was.
    void sl_reader(void* arg)
    {
        volatile uint64_t* const mine = static_cast<volatile uint64_t*>(arg);
        uintptr_t const below =
            reinterpret_cast<uintptr_t>(arg) - static_cast<uintptr_t>(mine[SL_GRAN]);
        mine[SL_ECHO] = *reinterpret_cast<volatile uint64_t const*>(below);
    }

    void t_task_handoff_slice()
    {
        settle_exits();
        uint64_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
        if (g == 0 or g > 0x10000u)
        {
            tap::skip("no granule to carve a two-page reservation from");
            return;
        }
        uint32_t const two = static_cast<uint32_t>(2u * g);
        void* const blk = kos_ram_alloc(two);
        if (blk == nullptr)
        {
            tap::skip("arena cannot spare a two-page reservation");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(blk, two, 0) == 0);
        volatile uint64_t* const lower = static_cast<volatile uint64_t*>(blk);
        lower[0] = SL_TOKEN;
        void* const upper = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(blk) + g);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(upper);
        out[SL_GRAN] = g;
        out[SL_ECHO] = 0;
        kos_task_t t = KOS_TASK_NONE;
        int const rc = kos_task_create(upper, static_cast<uint32_t>(g), 0, &t);
        if (rc == 0)
        {
            auto const rd = kos::thread::create(sl_reader, upper, "hsl", 10, KOS_POLICY_FIFO,
                                                0, false, nullptr, 0, nullptr, 0, nullptr, 0,
                                                nullptr, 0, 0, nullptr, t);
            if (rd.valid())
            {
                (void)rd.join();
            }
            (void)kos_task_kill(t);
            tap::diag("interior handoff admitted; the borrower read 0x%x below its page",
                      static_cast<unsigned>(out[SL_ECHO]));
        }
        TAP_CHECK(rc == -KOS_EPERM);
        // Control: accept the same reservation at its actual base.
        kos_task_t whole = KOS_TASK_NONE;
        if (kos_task_create(blk, two, 0, &whole) != 0)
        {
            tap::skip("task pool too small for the whole-reservation control");
            return;
        }
        TAP_CHECK(kos_task_kill(whole) == 0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // The donor task exits before its borrower, then a churn task allocates and writes frames:
    // the borrower's data must survive on the domain lifetime reference. Pool counters alone
    // cannot detect early reuse. main cannot serve as the donor; it joins it before the churn.
    enum
    {
        DX_STATUS = 0, // 1 once the donor seated a borrower into its own block
        DX_ADDR = 1,   // the donor's address for that block
        DX_TOKEN = 2,  // the frame under it, named in the donor's own space
        DX_HELD = 3,   // spaces held while the donor was still alive
        DX_WORDS = 4
    };
    enum
    {
        CX_TAKEN = 0,  // blocks the churn task got
        CX_TOKEN0 = 1, // one word per block: the frame it landed on
        CX_MAX = 24
    };
    constexpr uint32_t DX_BLK = 64;
    constexpr uint32_t DX_CBLK = 256;
    constexpr uint64_t DX_DONOR_WORD = 0xD0D0D0D0D0D0D0D0ull;
    constexpr uint64_t DX_BORROW_WORD = 0xB0B0B0B0B0B0B0B0ull;
    constexpr uint64_t DX_CHURN_WORD = 0xC0C0C0C0C0C0C0C0ull;
    constexpr uint32_t DX_CALL_US = 250000;
    constexpr int DX_EP = 2; // the donor's and the borrower's endpoint index
    struct DxReport
    {
        uint64_t seen;     // what the borrower found in the block
        uint64_t readback; // what it read after writing its own word over it
        uint64_t token;    // the frame under the block, named in the BORROWER's space
        uint64_t addr;
    };

    // Park until main has joined the donor and exhausted available frames with churn.
    void dx_borrower(void* arg) // caps: done@1, ep(WAIT)@2
    {
        volatile uint64_t* const blk = static_cast<volatile uint64_t*>(arg);
        char msg[sizeof(DxReport)] = {};
        struct kos_recv_info info = {0u, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, DX_EP, 0, KOS_TIMEOUT_NONE);
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, msg, kos_call_lens_pack(0, sizeof(msg)), &opts);
        info = opts.info;
        DxReport r = {};
        r.seen = blk[0];
        blk[0] = DX_BORROW_WORD; // and WRITES it: a stale mapping is writable, not only readable
        r.readback = blk[0];
        r.token = kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(arg));
        r.addr = reinterpret_cast<uintptr_t>(arg);
        if (got >= 0)
        {
            memcpy(msg, &r, sizeof(r));
            (void)kos_reply(info.reply_cap, msg, sizeof(msg));
        }
        kos_sem_post(CH_DONE);
    }

    // main joins the donor instead of waiting for a post.
    void dx_donor(void* arg) // caps: done@1, ep(WAIT|TRANSFER)@2; arg is main's report block
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        void* const blk = kos_ram_alloc(DX_BLK);
        if (blk == nullptr or kos_mem_self_grant(blk, DX_BLK, 0) != 0)
        {
            return;
        }
        volatile uint64_t* const mine = static_cast<volatile uint64_t*>(blk);
        mine[0] = DX_DONOR_WORD;
        out[DX_ADDR] = reinterpret_cast<uintptr_t>(blk);
        out[DX_TOKEN] =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(blk));
        kos_task_t tb = KOS_TASK_NONE;
        if (kos_task_create(blk, DX_BLK, 0, &tb) != 0)
        {
            return;
        }
        kos_cap_grant caps[] = {{CH_DONE, CH_FULL}, {DX_EP, KOS_CAP_WAIT}};
        if (not kos::thread::create_caps(dx_borrower, blk, "dxB", 10, caps, 2, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, 0, nullptr, tb)
                    .valid())
        {
            return;
        }
        // Read last: main compares it after the join with no space allocated or freed between.
        out[DX_HELD] = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
        out[DX_STATUS] = 1;
    }

    void dx_churn(void* arg) // caps: done@1
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        size_t const g = discover_granule();
        uint64_t taken = 0;
        while (g != 0 and taken < static_cast<uint64_t>(CX_MAX))
        {
            void* const p = kos_ram_alloc(static_cast<uint32_t>(g));
            if (p == nullptr or kos_mem_self_grant(p, static_cast<uint32_t>(g), 0) != 0)
            {
                break;
            }
            *static_cast<volatile uint64_t*>(p) = DX_CHURN_WORD;
            out[CX_TOKEN0 + taken] =
                kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(p));
            taken++;
        }
        out[CX_TAKEN] = taken;
        kos_sem_post(CH_DONE);
    }

    void t_task_handoff_donor_exits()
    {
        settle_exits();
        void* const dblk = kos_ram_alloc(DX_BLK);
        void* const cblk = kos_ram_alloc(DX_CBLK);
        if (dblk == nullptr or cblk == nullptr)
        {
            tap::skip("arena cannot spare the two report blocks");
            return;
        }
        if (kos_mem_self_grant(dblk, DX_BLK, 0) != 0
            or kos_mem_self_grant(cblk, DX_CBLK, 0) != 0)
        {
            tap::skip("the report blocks are not reachable");
            return;
        }
        volatile uint64_t* const dout = static_cast<volatile uint64_t*>(dblk);
        volatile uint64_t* const cout = static_cast<volatile uint64_t*>(cblk);
        for (int i = 0; i < DX_WORDS; i++)
        {
            dout[i] = 0;
        }
        for (int i = 0; i < CX_TOKEN0 + CX_MAX; i++)
        {
            cout[i] = 0;
        }
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("no endpoint slot for the borrower's report");
            return;
        }
        kos_task_t td = KOS_TASK_NONE;
        if (kos_task_create(dblk, DX_BLK, 0, &td) != 0)
        {
            (void)kos_handle_close(ep);
            tap::skip("task pool too small for the donor");
            return;
        }
        // TRANSFER lets the donor delegate the endpoint to the borrower.
        kos_cap_grant dcaps[] = {{g_done, CH_FULL},
                                 {ep, static_cast<uint8_t>(KOS_CAP_WAIT | KOS_CAP_TRANSFER)}};
        // The donor seats its borrower in a task of its own, which is the task authority.
        auto donor = kos::thread::create_caps(dx_donor, dblk, "dxD", 10, dcaps, 2,
                                              KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                              KOS_AUTH_MEMORY | KOS_AUTH_TASKS, nullptr, td);
        if (not donor.valid())
        {
            (void)kos_task_kill(td);
            (void)kos_handle_close(ep);
            tap::skip("thread pool too small for the donor");
            return;
        }
        // The donor has exited before any frame churn begins.
        int const jrc = donor.join();
        bool const seated = jrc == 0 and dout[DX_STATUS] == 1u;
        // Drop main's creator hold before the churn, or it hides a missing borrower reference.
        (void)kos_task_kill(td);
        if (not seated)
        {
            (void)kos_handle_close(ep);
            tap::skip("the donor could not seat a borrower");
            return;
        }
        uint64_t const held_after = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);

        kos_task_t tc = KOS_TASK_NONE;
        uint64_t churned = 0;
        if (kos_task_create(cblk, DX_CBLK, 0, &tc) == 0)
        {
            kos_cap_grant ccaps[] = {{g_done, CH_FULL}};
            if (kos::thread::create_caps(dx_churn, cblk, "dxC", 10, ccaps, 1, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, KOS_AUTH_MEMORY, nullptr, tc)
                    .valid())
            {
                wait_n(1);
                churned = cout[CX_TAKEN];
            }
            (void)kos_task_kill(tc);
        }

        // Release and receive in one call so a borrower that never parked fails promptly.
        char msg[sizeof(DxReport)] = {};
        int32_t const n = kos_call_timed(ep, msg, sizeof(msg), sizeof(msg), DX_CALL_US);
        DxReport r = {};
        if (n == static_cast<int32_t>(sizeof(r)))
        {
            memcpy(&r, msg, sizeof(r));
        }
        wait_n(1);
        (void)kos_handle_close(ep);

        // No frame the pool handed the churn task may be the one the borrower still maps.
        bool handed_out = false;
        uint64_t clo = 0;
        uint64_t chi = 0;
        for (uint64_t i = 0; i < churned and i < static_cast<uint64_t>(CX_MAX); i++)
        {
            uint64_t const tok = cout[CX_TOKEN0 + i];
            if (tok == dout[DX_TOKEN])
            {
                handed_out = true;
            }
            if (clo == 0 or tok < clo)
            {
                clo = tok;
            }
            if (tok > chi)
            {
                chi = tok;
            }
        }
        // Report the churn frame range: bytes outside it do not test frame reuse.
        tap::diag("donor-exits: donor frame %u, borrower frame %u, spaces held %u -> %u",
                  static_cast<unsigned>(dout[DX_TOKEN]), static_cast<unsigned>(r.token),
                  static_cast<unsigned>(dout[DX_HELD]), static_cast<unsigned>(held_after));
        tap::diag("donor-exits: borrower read %u, churn took %u blocks over frames %u..%u",
                  static_cast<unsigned>(r.seen), static_cast<unsigned>(churned),
                  static_cast<unsigned>(clo), static_cast<unsigned>(chi));
        // The donor's task has exited, but the borrower must still hold its domain.
        TAP_CHECK(held_after == dout[DX_HELD]);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r))); // the borrower answered at all
        TAP_CHECK(r.addr == dout[DX_ADDR]);              // at the donor's address
        TAP_CHECK(r.token != 0 and r.token == dout[DX_TOKEN]); // on the donor's frame
        TAP_CHECK(r.seen == DX_DONOR_WORD);
        TAP_CHECK(r.readback == DX_BORROW_WORD);
        TAP_CHECK(not handed_out);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // An ungranted reservation has no leaves, so only aspace_release returns its frames. The
    // first cycle is a warm-up.
    enum
    {
        RT_ADDR = 0, // the reservation the member took and never mapped
        RT_FREE = 1, // frames free with it out, read INSIDE the live process
        RT_WORDS = 2
    };
    constexpr uintptr_t RT_PAGES = 3;
    void rt_member(void*) // caps: E(SIGNAL)@1
    {
        uint64_t rep[RT_WORDS] = {0, 0};
        uintptr_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
        // Leave the reservation unmapped for teardown.
        void* const blk = kos_ram_alloc(static_cast<size_t>(RT_PAGES * g));
        rep[RT_ADDR] = reinterpret_cast<uintptr_t>(blk);
        rep[RT_FREE] = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        (void)kos_send(1, rep, sizeof(rep));
    }
    bool rt_cycle(kos_cap_t ep, uint64_t* rep)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            return false;
        }
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        auto m = kos::thread::create_caps(rt_member, nullptr, "rtres", 10, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                          KOS_AUTH_MEMORY, nullptr, t);
        if (not m.valid())
        {
            (void)kos_task_kill(t);
            return false;
        }
        // The member parks in its send until this receive arrives, so the report is read
        // while the reservation is still out.
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        bool const heard = kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(uint64_t) * RT_WORDS), &opts)
                           == static_cast<int32_t>(sizeof(uint64_t) * RT_WORDS);
        bool const joined = m.join() == 0;
        // main's creator hold keeps the empty task's space alive.
        bool const reaped = kos_task_kill(t) == 0;
        return heard and joined and reaped;
    }
    void t_reservation_teardown()
    {
        settle_exits();
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        uint64_t rep[RT_WORDS] = {0, 0};
        if (not rt_cycle(ep, rep))
        {
            (void)kos_handle_close(ep);
            tap::skip("task or thread pool too small to churn a reservation");
            return;
        }
        uint64_t const before = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        bool const ran = rt_cycle(ep, rep);
        uint64_t const after = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        (void)kos_handle_close(ep);
        TAP_CHECK(ran);
        TAP_CHECK(rep[RT_ADDR] != 0);
        tap::diag("reservation teardown: %u frames free, %u inside the live process, %u after",
                  static_cast<unsigned>(before), static_cast<unsigned>(rep[RT_FREE]),
                  static_cast<unsigned>(after));
        // Pool usage must change, or an empty reservation passes.
        TAP_CHECK(rep[RT_FREE] + RT_PAGES <= before);
        TAP_CHECK(after == before);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // Frames a dead process filled must read zeroed when reallocated. The warm-up cycle
    // equalizes retained table costs.
    enum
    {
        FS_FRAME = 0, // the frame the reservation's first page sits in
        FS_HITS = 1,  // token words found BEFORE this process wrote any
        FS_WORDS = 2
    };
    constexpr uintptr_t FS_PAGES = 2;
    constexpr uint64_t FS_TOKEN = 0x5CB0BE5CB0BE5CB0ull;
    void fs_member(void*) // caps: E(SIGNAL)@1
    {
        uint64_t rep[FS_WORDS] = {0, 0};
        uintptr_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
        size_t const bytes = static_cast<size_t>(FS_PAGES * g);
        void* const blk = kos_ram_alloc(bytes);
        if (blk != nullptr and kos_mem_self_grant(blk, bytes, 0) == 0)
        {
            rep[FS_FRAME] =
                kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(blk));
            volatile uint64_t* const w = static_cast<volatile uint64_t*>(blk);
            size_t const words = bytes / sizeof(uint64_t);
            // READ BEFORE WRITE: what the previous holder of these frames left behind.
            for (size_t i = 0; i < words; i++)
            {
                if (w[i] == FS_TOKEN)
                {
                    rep[FS_HITS]++;
                }
            }
            for (size_t i = 0; i < words; i++)
            {
                w[i] = FS_TOKEN;
            }
        }
        (void)kos_send(1, rep, sizeof(rep));
    }
    bool fs_cycle(kos_cap_t ep, uint64_t* rep)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            return false;
        }
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        auto m = kos::thread::create_caps(fs_member, nullptr, "fscrb", 10, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                          KOS_AUTH_MEMORY, nullptr, t);
        if (not m.valid())
        {
            (void)kos_task_kill(t);
            return false;
        }
        // The member parks in its send until this receive arrives, and the frames go back
        // only once the group is reaped below.
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        bool const heard = kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(uint64_t) * FS_WORDS), &opts)
                           == static_cast<int32_t>(sizeof(uint64_t) * FS_WORDS);
        bool const joined = m.join() == 0;
        bool const reaped = kos_task_kill(t) == 0;
        return heard and joined and reaped;
    }
    void t_frame_scrub_cross_task()
    {
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        uint64_t warm[FS_WORDS] = {0, 0};
        uint64_t first[FS_WORDS] = {0, 0};
        uint64_t second[FS_WORDS] = {0, 0};
        bool const ran = fs_cycle(ep, warm) and fs_cycle(ep, first) and fs_cycle(ep, second);
        (void)kos_handle_close(ep);
        if (not ran)
        {
            tap::skip("task or thread pool too small to churn a process");
            return;
        }
        tap::diag("frame scrub: frames %u then %u, token words read %u then %u",
                  static_cast<unsigned>(first[FS_FRAME]),
                  static_cast<unsigned>(second[FS_FRAME]),
                  static_cast<unsigned>(first[FS_HITS]),
                  static_cast<unsigned>(second[FS_HITS]));
        TAP_CHECK(first[FS_FRAME] != 0);
        // Reuse of the first process's frames, or stale data would not be visible.
        TAP_CHECK(second[FS_FRAME] == first[FS_FRAME]);
        TAP_CHECK(second[FS_HITS] == 0);
    }

    // A spawn refused for thread slots after task/domain/space creation must release those
    // objects and the donor reference. A separate donor task, so its lifetime ends in this arm.
    constexpr int LR_PARK_CAP = 24;
    enum
    {
        LR_RC = 0,      // what the refused spawn answered, negated
        LR_PARKED = 1,  // workers seated before the pool refused
        LR_SPACES0 = 2, // spaces held and frames free, read either side of the refusal
        LR_SPACES1 = 3,
        LR_FRAMES0 = 4,
        LR_FRAMES1 = 5,
        LR_WORDS = 6
    };
    void lr_parked(void*) // caps: gate@1
    {
        kos_sem_wait(1);
    }
    void lr_member(void*) // caps: E(SIGNAL)@1
    {
        uint64_t rep[LR_WORDS] = {0, 0, 0, 0, 0, 0};
        uintptr_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
        uint32_t const bytes = static_cast<uint32_t>(2u * g);
        void* const blk = kos_ram_alloc(bytes);
        kos_cap_t gate = KOS_CAP_NONE;
        if (blk != nullptr and kos_mem_self_grant(blk, bytes, 0) == 0
            and kos_sem_create(0, &gate) == 0)
        {
            kos_thread_t held[LR_PARK_CAP];
            kos_cap_grant gcaps[] = {{gate, CH_FULL}};
            int n = 0;
            // Plain spawns consume thread slots while sharing the member's task and space.
            while (n < LR_PARK_CAP)
            {
                auto h = kos::thread::create_caps(lr_parked, nullptr, "lrprk", 10, gcaps, 1);
                if (not h.valid())
                {
                    break;
                }
                held[n] = h.id();
                n++;
            }
            rep[LR_PARKED] = static_cast<uint64_t>(n);
            rep[LR_SPACES0] = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
            rep[LR_FRAMES0] = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
            int const rc = kos::thread::create(lr_parked, nullptr, "lrbad", 10,
                                               KOS_POLICY_FIFO, 0, false, blk, bytes).error();
            rep[LR_RC] = static_cast<uint64_t>(static_cast<uint32_t>(-rc));
            rep[LR_SPACES1] = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
            rep[LR_FRAMES1] = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
            for (int i = 0; i < n; i++)
            {
                (void)kos_sem_post(gate);
            }
            for (int i = 0; i < n; i++)
            {
                (void)kos_thread_join(held[i], KOS_TIMEOUT_NONE);
            }
            (void)kos_handle_close(gate);
        }
        (void)kos_send(1, rep, sizeof(rep));
    }
    bool lr_cycle(kos_cap_t ep, uint64_t* rep)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            return false;
        }
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        // The member's spawn builds a task, so it holds the task authority and is refused
        // further down, where this arm looks.
        auto m = kos::thread::create_caps(lr_member, nullptr, "lrful", 10, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                          KOS_AUTH_MEMORY | KOS_AUTH_TASKS, nullptr, t);
        if (not m.valid())
        {
            (void)kos_task_kill(t);
            return false;
        }
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        bool const heard = kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(uint64_t) * LR_WORDS), &opts)
                           == static_cast<int32_t>(sizeof(uint64_t) * LR_WORDS);
        bool const joined = m.join() == 0;
        bool const reaped = kos_task_kill(t) == 0;
        return heard and joined and reaped;
    }
    void t_spawn_refusal_frees_task()
    {
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        uint64_t rep[LR_WORDS] = {0, 0, 0, 0, 0, 0};
        bool const ran = lr_cycle(ep, rep);
        (void)kos_handle_close(ep);
        if (not ran)
        {
            tap::skip("task or thread pool too small to fill from a member");
            return;
        }
        tap::diag("thread-pool refusal: %u parked, rc %u, spaces %u->%u, frames %u->%u",
                  static_cast<unsigned>(rep[LR_PARKED]), static_cast<unsigned>(rep[LR_RC]),
                  static_cast<unsigned>(rep[LR_SPACES0]),
                  static_cast<unsigned>(rep[LR_SPACES1]),
                  static_cast<unsigned>(rep[LR_FRAMES0]),
                  static_cast<unsigned>(rep[LR_FRAMES1]));
        if (rep[LR_PARKED] == 0 or rep[LR_PARKED] == LR_PARK_CAP)
        {
            // At the cap the pool outran this arm, so the refusal it measures never happened
            // and the two equalities below would hold vacuously.
            tap::skip("thread pool not exhausted by this arm");
            return;
        }
        TAP_CHECK(rep[LR_RC] == KOS_ENOMEM);
        TAP_CHECK(rep[LR_SPACES1] == rep[LR_SPACES0]);
        TAP_CHECK(rep[LR_FRAMES1] == rep[LR_FRAMES0]);
    }

    // A failed spawn must release both the borrower space and its donor reference. The member's
    // own task is the donor; no domain is allocated across the reap, which could hide a stale
    // reference.
    enum
    {
        LD_RC = 0,     // the refused spawn's own answer, negated
        LD_SPACES = 1, // spaces held, read inside the member AFTER the refusal
        LD_FRAMES = 2,
        LD_WORDS = 3
    };
    void ld_never(void*)
    {
        kos_exit(0);
    }
    void ld_member(void*) // caps: E(SIGNAL)@1, done@2
    {
        uint64_t rep[LD_WORDS] = {0, 0, 0};
        uintptr_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
        uint32_t const bytes = static_cast<uint32_t>(2u * g);
        void* const blk = kos_ram_alloc(bytes);
        int rc = 0;
        if (blk != nullptr and kos_mem_self_grant(blk, bytes, 0) == 0)
        {
            kos_cap_grant caps[] = {{CH_LOCK, CH_FULL}};
            uint16_t const dest = 0x0FFFu; // past any child capability run
            rc = kos::thread::create_caps(ld_never, nullptr, "ldbad", 10, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, blk, bytes, 0,
                                          &dest).error();
        }
        rep[LD_RC] = static_cast<uint64_t>(static_cast<uint32_t>(-rc));
        rep[LD_SPACES] = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
        rep[LD_FRAMES] = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        (void)kos_send(1, rep, sizeof(rep));
    }
    void t_spawn_refusal_frees_donor()
    {
        settle_exits();
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        uint64_t rep[LD_WORDS] = {0, 0, 0};
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            (void)kos_handle_close(ep);
            tap::skip("task pool too small");
            return;
        }
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}, {g_done, CH_FULL}};
        // The member's spawn builds a task, so it holds the task authority and is refused
        // further down, where this arm looks.
        auto m = kos::thread::create_caps(ld_member, nullptr, "lddon", 10, caps, 2,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                          KOS_AUTH_MEMORY | KOS_AUTH_TASKS, nullptr, t);
        if (not m.valid())
        {
            (void)kos_task_kill(t);
            (void)kos_handle_close(ep);
            tap::skip("thread pool too small");
            return;
        }
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        bool const heard = kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(uint64_t) * LD_WORDS), &opts)
                           == static_cast<int32_t>(sizeof(uint64_t) * LD_WORDS);
        bool const joined = m.join() == 0;
        // The member's group dies here, and its domain with it unless something still
        // holds a reference on it.
        bool const reaped = kos_task_kill(t) == 0;
        uint64_t const spaces_end = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
        uint64_t const frames_end = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        (void)kos_handle_close(ep);
        tap::diag("late refusal in a donor: rc %u, spaces %u->%u, frames %u->%u",
                  static_cast<unsigned>(rep[LD_RC]),
                  static_cast<unsigned>(rep[LD_SPACES]),
                  static_cast<unsigned>(spaces_end),
                  static_cast<unsigned>(rep[LD_FRAMES]),
                  static_cast<unsigned>(frames_end));
        TAP_CHECK(heard and joined and reaped);
        // The refusal really was the late one, and not an early argument check.
        TAP_CHECK(rep[LD_RC] == KOS_EINVAL);
        // EXACTLY ONE space fewer: the donor's, which the reap can only release if the
        // refused handoff gave its reference back.
        TAP_CHECK(spaces_end + 1u == rep[LD_SPACES]);
        TAP_CHECK(frames_end > rep[LD_FRAMES]);
    }

    // A task sibling overwrites the victim's user stack with privileged state before it runs;
    // the victim must still reach its own entry and fail kos_shutdown with EPERM. The whole
    // frame window is filled so a layout change cannot hide the attack, and the sibling stays
    // alive until the victim finishes.
    constexpr uint64_t HOSTILE_EL1H = 0x205u; // M[3:0] = EL1h, plus the debug mask
    constexpr uint32_t HOSTILE_WINDOW = 1024; // spans the whole armv8a exception frame
    constexpr int CH_HPARK = 2; // delegated SECOND to the sibling
    // The victim has private globals, so report through the block shared with main.
    void hostile_victim(void* arg)
    {
        *static_cast<volatile int32_t*>(arg) = kos_shutdown(0);
        kos_exit(0);
    }
    void hostile_sibling(void* arg) // caps: done@1, park@2
    {
        uintptr_t const top = reinterpret_cast<uintptr_t>(arg);
        volatile uint64_t* const w = reinterpret_cast<volatile uint64_t*>(top - HOSTILE_WINDOW);
        for (uint32_t i = 0; i < HOSTILE_WINDOW / sizeof(uint64_t); i++)
        {
            w[i] = HOSTILE_EL1H;
        }
        kos_sem_post(CH_DONE);
        kos_sem_wait(CH_HPARK);
        kos_exit(1); // unreachable: nothing posts that semaphore
    }
    void t_parked_frame_hostile()
    {
        TAP_SKIP_ONE_CORE_ORDER();
#if defined(KICKOS_TLS) && KICKOS_TLS
        // The TLS seat admits a caller stack of exactly one stride, stride-aligned.
        constexpr uint32_t VSTK = KICKOS_TLS_STRIDE;
#else
        constexpr uint32_t VSTK = 8192;
#endif
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        // Allow one aligned stack plus a separate verdict slot regardless of allocation base.
        void* const raw = kos_ram_alloc(3u * VSTK);
        if (raw == nullptr)
        {
            (void)kos_handle_close(park);
            tap::skip("arena cannot spare a strided victim stack");
            return;
        }
        // Map main's reservation so it can read the shared verdict.
        TAP_CHECK(kos_mem_self_grant(raw, 3u * VSTK, 0) == 0);
        uintptr_t const vbase = (reinterpret_cast<uintptr_t>(raw) + (VSTK - 1u))
            & ~static_cast<uintptr_t>(VSTK - 1u);
        volatile int32_t* const verdict =
            reinterpret_cast<volatile int32_t*>(reinterpret_cast<uintptr_t>(raw) + 2u * VSTK);
        *verdict = -99;
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(raw, 3u * VSTK, 0, &task) == 0);
        // Lower priority keeps the victim parked until main joins. A caller-owned stack lets its
        // sibling locate and overwrite the saved frame.
        auto const victim = kos::thread::create(hostile_victim,
                                                const_cast<int32_t*>(verdict), "hvic", 1,
                                                KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                reinterpret_cast<void*>(vbase), VSTK,
                                                nullptr, 0, nullptr, 0, 0, nullptr, task);
        if (not victim.valid())
        {
            (void)kos_task_kill(task);
            (void)kos_handle_close(park);
            tap::skip("pool too small for the victim");
            return;
        }
        kos_cap_grant const caps[2] = {{g_done, CH_FULL}, {park, KOS_CAP_WAIT}};
        auto const sibling = kos::thread::create(hostile_sibling,
                                               reinterpret_cast<void*>(vbase + VSTK), "hsib",
                                               10, KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                               nullptr, 0, nullptr, 0, caps, 2, 0, nullptr,
                                               task);
        if (not sibling.valid())
        {
            (void)kos_task_kill(task);
            (void)victim.join();
            (void)kos_handle_close(park);
            tap::skip("pool too small for the sibling");
            return;
        }
        wait_n(1); // the scribble is COMPLETE before the victim is let go
        int const jrc = victim.join();
        int const rc = *verdict;
        (void)kos_task_kill(task);
        (void)sibling.join();
        (void)kos_handle_close(park);
        TAP_CHECK(jrc == 0);
        TAP_CHECK(rc == -KOS_EPERM);
    }

    // Copy across page boundaries in the specified owner's space.

    // The probe maps adjacent virtual pages to nonadjacent frames and checks the physical
    // neighbor: a single memcpy from the translated base would write there instead of crossing
    // to the next virtual page.
    void t_split_access()
    {
        uint64_t const bits = kos_aspace_probe(KOS_ASPACE_OP_SPLIT_ACCESS, 0);
        tap::diag("split access: bits %u of %u", static_cast<unsigned>(bits),
                  static_cast<unsigned>(KOS_ASPACE_SPLIT_ALL));
        TAP_CHECK(bits == KOS_ASPACE_SPLIT_ALL);
    }

    // Processes whose static buffers have equal virtual addresses but different frames. The
    // sender's receive-info guard detects a copy to the current space instead of the parked
    // receiver's.
    enum
    {
        PI_ADDR = 0,      // the member's own &g_pi_msg[0]
        PI_INFO_ADDR = 1, // its own &g_pi_info
        PI_FRAME = 2,     // the frame under its payload buffer
        PI_N = 3,         // what its own IPC call returned
        PI_SEEN = 4,      // payload bytes matching the pattern its role expects
        PI_BADGE = 5,
        PI_RCAP = 6,
        PI_REPLY_RC = 7, // the call arm's server only
        PI_WORDS = 8
    };
    constexpr int PI_MSG = 12;
    constexpr uint32_t PI_GUARD = 0xDEADBEEFu;
    constexpr uint32_t PI_BLK = 256;
    // Volatile detects writes by the other process or kernel through an incorrect alias.
    volatile char g_pi_msg[PI_MSG] = {};
    volatile kos_recv_info g_pi_info = {};

    void pi_mine(volatile uint64_t* out, char first)
    {
        for (int i = 0; i < PI_MSG; i++)
        {
            g_pi_msg[i] = static_cast<char>(first + i);
        }
        g_pi_info.badge = PI_GUARD;
        g_pi_info.reply_cap = PI_GUARD;
        out[PI_ADDR] = reinterpret_cast<uintptr_t>(&g_pi_msg[0]);
        out[PI_INFO_ADDR] = reinterpret_cast<uintptr_t>(&g_pi_info);
        out[PI_FRAME] = kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT,
                                         reinterpret_cast<uintptr_t>(&g_pi_msg[0]));
    }

    void pi_report(volatile uint64_t* out, int32_t n, char first)
    {
        out[PI_N] = static_cast<uint64_t>(static_cast<int64_t>(n));
        uint64_t seen = 0;
        for (int i = 0; i < PI_MSG; i++)
        {
            if (g_pi_msg[i] == static_cast<char>(first + i))
            {
                seen++;
            }
        }
        out[PI_SEEN] = seen;
        out[PI_BADGE] = g_pi_info.badge;
        out[PI_RCAP] = g_pi_info.reply_cap;
    }

    void pi_server(void* arg) // caps: done@1, E(WAIT)@2
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, '\0');
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, 2, 0, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, const_cast<char*>(&g_pi_msg[0]),
                                         kos_call_lens_pack(0, PI_MSG), &o);
        *const_cast<kos_recv_info*>(&g_pi_info) = o.info;
        pi_report(out, n, 'A');
        kos_sem_post(CH_DONE);
    }

    void pi_client(void* arg) // caps: done@1, E(SIGNAL)@2
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, 'A');
        int32_t const n = kos_send(2, const_cast<char*>(&g_pi_msg[0]), PI_MSG);
        pi_report(out, n, 'A');
        kos_sem_post(CH_DONE);
    }

    void pi_call_server(void* arg) // caps: done@1, E(WAIT)@2
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, '\0');
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, 2, 0, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, const_cast<char*>(&g_pi_msg[0]),
                                         kos_call_lens_pack(0, PI_MSG), &o);
        *const_cast<kos_recv_info*>(&g_pi_info) = o.info;
        pi_report(out, n, 'A');
        // The reply leaves the server's OWN copy of the buffer and must land in the parked
        // caller's copy, at the same number.
        for (int i = 0; i < PI_MSG; i++)
        {
            g_pi_msg[i] = static_cast<char>('a' + i);
        }
        out[PI_REPLY_RC] = static_cast<uint64_t>(static_cast<int64_t>(
            kos_reply(g_pi_info.reply_cap, const_cast<char*>(&g_pi_msg[0]), PI_MSG)));
        kos_sem_post(CH_DONE);
    }

    void pi_call_client(void* arg) // caps: done@1, E(SIGNAL)@2
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, 'A');
        int32_t const n = kos_call(2, const_cast<char*>(&g_pi_msg[0]), PI_MSG, PI_MSG);
        pi_report(out, n, 'a'); // the REPLY, in its own copy of the one address
        kos_sem_post(CH_DONE);
    }

    // The server's higher priority parks it before the client sends, so delivery goes to a
    // parked peer and not through the sender queue.
    bool pi_two_processes(void (*server)(void*), void (*client)(void*),
                          volatile uint64_t** oa, volatile uint64_t** ob)
    {
        *oa = nullptr;
        *ob = nullptr;
        void* const ba = kos_ram_alloc(PI_BLK);
        void* const bb = kos_ram_alloc(PI_BLK);
        if (ba == nullptr or bb == nullptr)
        {
            return false;
        }
        if (kos_mem_self_grant(ba, PI_BLK, 0) != 0 or kos_mem_self_grant(bb, PI_BLK, 0) != 0)
        {
            return false;
        }
        volatile uint64_t* const sa = static_cast<volatile uint64_t*>(ba);
        volatile uint64_t* const sb = static_cast<volatile uint64_t*>(bb);
        for (int i = 0; i < PI_WORDS; i++)
        {
            sa[i] = 0;
            sb[i] = 0;
        }
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            return false;
        }
        kos_task_t ta = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        if (kos_task_create(ba, PI_BLK, 0, &ta) != 0)
        {
            (void)kos_handle_close(ep);
            return false;
        }
        if (kos_task_create(bb, PI_BLK, 0, &tb) != 0)
        {
            (void)kos_task_kill(ta);
            (void)kos_handle_close(ep);
            return false;
        }
        kos_cap_grant const scaps[2] = {{g_done, CH_FULL}, {ep, KOS_CAP_WAIT}};
        kos_cap_grant const ccaps[2] = {{g_done, CH_FULL}, {ep, KOS_CAP_SIGNAL}};
        // The client starts only after the server; an unmatched client would park forever.
        bool const s_ok = kos::thread::create_caps(server, ba, "piS", 12, scaps, 2,
                                                   KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                                   nullptr, ta).valid();
        bool c_ok = false;
        if (s_ok)
        {
            c_ok = kos::thread::create_caps(client, bb, "piC", 11, ccaps, 2, KOS_POLICY_FIFO,
                                            0, false, nullptr, 0, 0, nullptr, tb).valid();
        }
        if (s_ok and not c_ok)
        {
            // main sends in the client's place so the server finishes within this arm.
            char pad[PI_MSG] = {};
            (void)kos_send(ep, pad, PI_MSG);
        }
        int seated = 0;
        if (s_ok)
        {
            seated++;
        }
        if (c_ok)
        {
            seated++;
        }
        wait_n(seated);
        (void)kos_task_kill(ta);
        (void)kos_task_kill(tb);
        (void)kos_handle_close(ep);
        if (seated != 2)
        {
            return false;
        }
        *oa = sa;
        *ob = sb;
        return true;
    }

    void pi_root_seed()
    {
        for (int i = 0; i < PI_MSG; i++)
        {
            g_pi_msg[i] = static_cast<char>('R' + i);
        }
        g_pi_info.badge = PI_GUARD;
        g_pi_info.reply_cap = PI_GUARD;
    }

    // main's own copies of both, which no member's IPC may reach.
    bool pi_root_intact()
    {
        bool ok = g_pi_info.badge == PI_GUARD and g_pi_info.reply_cap == PI_GUARD;
        for (int i = 0; i < PI_MSG; i++)
        {
            ok = ok and g_pi_msg[i] == static_cast<char>('R' + i);
        }
        return ok;
    }

    void t_process_ipc_same_addr()
    {
        volatile uint64_t* oa = nullptr;
        volatile uint64_t* ob = nullptr;
        pi_root_seed();
        if (not pi_two_processes(pi_server, pi_client, &oa, &ob))
        {
            tap::skip("thread, task, arena or endpoint pool too small for 2 processes");
            return;
        }
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pi_msg[0]);
        uintptr_t const own_info = reinterpret_cast<uintptr_t>(&g_pi_info);
        tap::diag("same-address send: frames %u/%u, n %d/%d",
                  static_cast<unsigned>(oa[PI_FRAME]), static_cast<unsigned>(ob[PI_FRAME]),
                  static_cast<int>(static_cast<int32_t>(oa[PI_N])),
                  static_cast<int>(static_cast<int32_t>(ob[PI_N])));
        TAP_CHECK(oa[PI_ADDR] == own and ob[PI_ADDR] == own);
        TAP_CHECK(oa[PI_INFO_ADDR] == own_info and ob[PI_INFO_ADDR] == own_info);
        TAP_CHECK(oa[PI_FRAME] != 0 and ob[PI_FRAME] != 0);
        TAP_CHECK(oa[PI_FRAME] != ob[PI_FRAME]);
        TAP_CHECK(static_cast<int32_t>(oa[PI_N]) == PI_MSG);
        TAP_CHECK(oa[PI_SEEN] == PI_MSG);
        // A plain send: badge 0 and no reply cap.
        TAP_CHECK(oa[PI_BADGE] == 0);
        TAP_CHECK(static_cast<uint32_t>(oa[PI_RCAP]) == KOS_CAP_NONE);
        // The sender's same-address buffer and receive-info must remain untouched.
        TAP_CHECK(static_cast<int32_t>(ob[PI_N]) == PI_MSG);
        TAP_CHECK(ob[PI_SEEN] == PI_MSG);
        TAP_CHECK(ob[PI_BADGE] == PI_GUARD);
        TAP_CHECK(static_cast<uint32_t>(ob[PI_RCAP]) == PI_GUARD);
        TAP_CHECK(pi_root_intact());
    }

    // With CALL: the request and reply cap land in the server's space, then the reply in the
    // parked caller's.
    void t_process_call_reply()
    {
        volatile uint64_t* oa = nullptr;
        volatile uint64_t* ob = nullptr;
        pi_root_seed();
        if (not pi_two_processes(pi_call_server, pi_call_client, &oa, &ob))
        {
            tap::skip("thread, task, arena or endpoint pool too small for 2 processes");
            return;
        }
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pi_msg[0]);
        tap::diag("same-address call: frames %u/%u, reply rc %d, n %d",
                  static_cast<unsigned>(oa[PI_FRAME]), static_cast<unsigned>(ob[PI_FRAME]),
                  static_cast<int>(static_cast<int32_t>(oa[PI_REPLY_RC])),
                  static_cast<int>(static_cast<int32_t>(ob[PI_N])));
        TAP_CHECK(oa[PI_ADDR] == own and ob[PI_ADDR] == own);
        TAP_CHECK(oa[PI_FRAME] != 0 and ob[PI_FRAME] != 0);
        TAP_CHECK(oa[PI_FRAME] != ob[PI_FRAME]);
        // Server side: the whole request and a reply cap, in its own space.
        TAP_CHECK(static_cast<int32_t>(oa[PI_N]) == PI_MSG);
        TAP_CHECK(oa[PI_SEEN] == PI_MSG);
        TAP_CHECK(oa[PI_BADGE] == 0);
        TAP_CHECK(static_cast<uint32_t>(oa[PI_RCAP]) != KOS_CAP_NONE
                  and static_cast<uint32_t>(oa[PI_RCAP]) != PI_GUARD);
        TAP_CHECK(static_cast<int32_t>(oa[PI_REPLY_RC]) == 0);
        // Caller side: the reply landed in ITS copy of the one address, and its own
        // out-pointer was never a target.
        TAP_CHECK(static_cast<int32_t>(ob[PI_N]) == PI_MSG);
        TAP_CHECK(ob[PI_SEEN] == PI_MSG);
        TAP_CHECK(ob[PI_BADGE] == PI_GUARD);
        TAP_CHECK(static_cast<uint32_t>(ob[PI_RCAP]) == PI_GUARD);
        TAP_CHECK(pi_root_intact());
    }

    // Access-denial tests.

#if KICKOS_FAULT_ISOLATION
    // Faults through an ungranted reservation owned by main, which no worker space maps.
    void fault_toucher(void* arg)
    {
        *static_cast<volatile unsigned char*>(arg) = 1u;
        // Unreachable: the write above ends this thread's whole TASK.
        kos_exit(1);
    }

    // Park on a semaphore that is never posted; only task-wide death can release it.
    void fault_sibling(void*) // caps: park@1
    {
        kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0);
        kos_exit(1);
    }

    // A user fault must kill all task siblings, which share the damaged space.
    // main must survive in its separate space.
    void t_fault_kills_task()
    {
        settle_exits();
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        void* const absent = kos_ram_alloc(64);
        if (absent == nullptr)
        {
            (void)kos_handle_close(park);
            tap::skip("no reservation left to name an unmapped page with");
            return;
        }
        // After main's reservation and before the victim's task, so the delta is that task alone.
        uint64_t const frames_before = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        kos_task_t task = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &task) != 0)
        {
            (void)kos_handle_close(park);
            tap::skip("no task slot");
            return;
        }
        kos_cap_grant const caps[1] = {{park, KOS_CAP_WAIT}};
        auto const sibling = kos::thread::create(fault_sibling, nullptr, "fsib", 10,
                                                KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                nullptr, 0, nullptr, 0, nullptr, 0, caps, 1,
                                                /*authority=*/0, /*cap_dest=*/nullptr, task);
        if (not sibling.valid())
        {
            (void)kos_task_kill(task);
            (void)kos_handle_close(park);
            tap::skip("pool too small for the sibling");
            return;
        }
        auto const victim = kos::thread::create(fault_toucher, absent, "fvic", 10,
                                               KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                               nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
                                               /*authority=*/0, /*cap_dest=*/nullptr, task);
        if (not victim.valid())
        {
            (void)kos_task_kill(task);
            (void)sibling.join();
            (void)kos_handle_close(park);
            tap::skip("pool too small for the victim");
            return;
        }
        tap::diag("faulting on 0x%lx, reserved by main and mapped in no space",
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(absent)));
        // Both joins must succeed without timeout; reaching this check proves main survived.
        TAP_CHECK(victim.join(STALL_TOLERANT_US) == 0);
        TAP_CHECK(sibling.join(STALL_TOLERANT_US) == 0);
        // Drop main's creator hold before checking pool counts; it keeps an empty task alive.
        TAP_CHECK(kos_task_kill(task) == 0);
        uint64_t const frames_after = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        tap::diag("frame pool free %u before the task, %u after it died",
                  static_cast<unsigned>(frames_before), static_cast<unsigned>(frames_after));
        TAP_CHECK(frames_after == frames_before);
        TAP_CHECK(kos_handle_close(park) == 0);
    }

    // Reads a kernel structure ring 3 must not reach; the read ends this thread's whole task.
    void kernel_state_reader(void* arg)
    {
        (void)*static_cast<unsigned char const volatile*>(arg);
        kos_exit(1);
    }

    // A space's own translation root and a core's per-core block are kernel memory the trap
    // path uses on every entry. A ring-3 read of either must end the reading task, and only
    // task-wide death releases its parked sibling, so a read that returned fails the join.
    void t_kernel_state_unreachable()
    {
        unsigned probed = 0;
        for (uint32_t which = 0; which < 2; ++which)
        {
            uint64_t const addr = kos_aspace_probe(KOS_ASPACE_OP_KERNEL_STATE, which);
            if (addr == 0)
            {
                continue; // the arch keeps no such structure apart from the kernel's data
            }
            settle_exits();
            kos_cap_t park = KOS_CAP_NONE;
            if (kos_sem_create(0, &park) != 0)
            {
                tap::skip("no semaphore slot");
                return;
            }
            kos_task_t task = KOS_TASK_NONE;
            if (kos_task_create(nullptr, 0, 0, &task) != 0)
            {
                (void)kos_handle_close(park);
                tap::skip("no task slot");
                return;
            }
            kos_cap_grant const caps[1] = {{park, KOS_CAP_WAIT}};
            auto const sibling = kos::thread::create(fault_sibling, nullptr, "ksib", 10,
                                                    KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                    nullptr, 0, nullptr, 0, nullptr, 0, caps, 1,
                                                    /*authority=*/0, /*cap_dest=*/nullptr, task);
            void* const target = reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
            auto const victim = kos::thread::create(kernel_state_reader, target, "kvic", 10,
                                                   KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                   nullptr, 0, nullptr, 0, nullptr, 0, nullptr,
                                                   0, /*authority=*/0, /*cap_dest=*/nullptr,
                                                   task);
            if (not sibling.valid() or not victim.valid())
            {
                (void)kos_task_kill(task);
                (void)kos_handle_close(park);
                tap::skip("pool too small for the reader and its sibling");
                return;
            }
            char const* what = "translation root";
            if (which == 1)
            {
                what = "per-core block";
            }
            tap::diag("%s at 0x%lx, read from ring 3", what, static_cast<unsigned long>(addr));
            TAP_CHECK(victim.join(STALL_TOLERANT_US) == 0);
            TAP_CHECK(sibling.join(STALL_TOLERANT_US) == 0);
            TAP_CHECK(kos_task_kill(task) == 0);
            TAP_CHECK(kos_handle_close(park) == 0);
            ++probed;
        }
        TAP_CHECK(probed > 0);
    }

#if defined(__x86_64__)
    // --- The x86 port grant --------------------------------------------------------------
    // A holder of the CMOS pair has the kernel write the index, which the chip keeps closed to
    // it, and reads the data port itself; a value with the NMI-mask bit is refused, and so is a
    // port it does not hold. A second holder and the PIC are refused, and COM1 is granted, or
    // refused as held where a console driver holds it. A COM2 holder
    // alternates with it on one core, each reading its own port, then reaches for the CMOS
    // data port and faults; the CMOS holder moves to another core where there is one and reads
    // again. A holder writing the CMOS index itself faults. Reports travel over an endpoint.
    constexpr uint16_t PW_CMOS = 0x70u;
    constexpr uint16_t PW_COM2 = 0x2f8u;
    constexpr uint32_t PW_JOIN_US = 500000;
    constexpr int PW_ROUNDS = 4;
    uint8_t pw_inb(uint16_t port)
    {
        uint8_t v = 0;
        __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
        return v;
    }
    void pw_outb(uint16_t port, uint8_t v)
    {
        __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port) : "memory");
    }
    struct PwSeen
    {
        int32_t index;
        int32_t nmi;
        int32_t unheld;
        int32_t rounds;
        int32_t moved; // this thread's own move to core 1, where there is one
    };
    void pw_cmos(void*) // caps: E(SIGNAL)@1, mine@2, theirs@3
    {
        PwSeen seen = {-99, -99, -99, 0, 0};
        seen.index = kos_port_reg_write(PW_CMOS, 0, 0x0au);
        seen.nmi = kos_port_reg_write(PW_CMOS, 0, 0x8au);
        seen.unheld = kos_port_reg_write(PW_COM2, 0, 0);
        for (int i = 0; i < PW_ROUNDS; i++)
        {
            kos_sem_wait(2);
            (void)pw_inb(PW_CMOS + 1u);
            seen.rounds++;
            kos_sem_post(3);
        }
        kos_sem_wait(2);
#if KICKOS_KERNEL_CORES > 1
        seen.moved = kos::thread::pin(kos_thread_self(), 1);
#endif
        (void)pw_inb(PW_CMOS + 1u);
        seen.rounds++;
        (void)kos_send(1, &seen, sizeof(seen));
        kos_exit(0);
    }
    void pw_com2(void*) // caps: E(SIGNAL)@1, mine@2, theirs@3
    {
        int32_t rounds = 0;
        for (int i = 0; i < PW_ROUNDS; i++)
        {
            kos_sem_wait(2);
            (void)pw_inb(PW_COM2);
            rounds++;
            if (i + 1 < PW_ROUNDS)
            {
                kos_sem_post(3);
            }
        }
        (void)kos_send(1, &rounds, sizeof(rounds));
        (void)pw_inb(PW_CMOS + 1u);
        (void)kos_send(1, &rounds, sizeof(rounds)); // unreachable: that port is not its own
        kos_exit(0);
    }
    void pw_index(void*) // caps: E(SIGNAL)@1
    {
        pw_outb(PW_CMOS, 0x0au);
        char const x = 1;
        (void)kos_send(1, &x, 1); // unreachable: the chip keeps the index closed
        kos_exit(0);
    }
    void pw_noop(void*) {}
    int32_t pw_recv(kos_cap_t ep, void* buf, size_t len, uint32_t timeout_us)
    {
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, timeout_us);
        return kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, len), &o);
    }
    void t_port_window()
    {
        settle_exits();
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t sa = KOS_CAP_NONE;
        kos_cap_t sb = KOS_CAP_NONE;
        kos_task_t tc = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        kos_task_t ti = KOS_TASK_NONE;
        if (kos_endpoint_create(&ep) != 0 or kos_sem_create(0, &sa) != 0
            or kos_sem_create(0, &sb) != 0 or kos_task_create(nullptr, 0, 0, &tc) != 0
            or kos_task_create(nullptr, 0, 0, &tb) != 0
            or kos_task_create(nullptr, 0, 0, &ti) != 0)
        {
            tap::skip("endpoint, semaphore or task pool too small");
            return;
        }
        kos_window const cmos = {PW_CMOS, 2u, KOS_WINDOW_PORTS, 0};
        kos_window const com2 = {PW_COM2, 8u, KOS_WINDOW_PORTS, 0};
        kos_window const pic = {0x20u, 2u, KOS_WINDOW_PORTS, 0};
        kos_cap_grant const ccaps[] = {{ep, KOS_CAP_SIGNAL}, {sa, CH_FULL}, {sb, CH_FULL}};
        kos_cap_grant const bcaps[] = {{ep, KOS_CAP_SIGNAL}, {sb, CH_FULL}, {sa, CH_FULL}};
        auto const holder = kos::thread::create(pw_cmos, nullptr, "pwc", 10, KOS_POLICY_FIFO,
                                                0, false, nullptr, 0, nullptr, 0, &cmos, 1,
                                                ccaps, 3, 0, nullptr, tc, 1u);
        TAP_CHECK(kos::thread::create(pw_noop, nullptr, "pwd", 10, KOS_POLICY_FIFO, 0, false,
                                      nullptr, 0, nullptr, 0, &cmos, 1, nullptr, 0, 0, nullptr,
                                      ti)
                      .error()
                  == -KOS_EBUSY);
        TAP_CHECK(kos::thread::create(pw_noop, nullptr, "pwp", 10, KOS_POLICY_FIFO, 0, false,
                                      nullptr, 0, nullptr, 0, &pic, 1, nullptr, 0, 0, nullptr,
                                      ti)
                      .error()
                  == -KOS_EINVAL);
        auto const other = kos::thread::create(pw_com2, nullptr, "pwb", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0, &com2, 1, bcaps,
                                               3, 0, nullptr, tb, 1u);
        TAP_CHECK(holder.valid() and other.valid());
        if (not holder.valid() or not other.valid())
        {
            return;
        }
        kos_sem_post(sa);
        int32_t rounds = -1;
        TAP_CHECK(pw_recv(ep, &rounds, sizeof(rounds), PW_JOIN_US)
                  == static_cast<int32_t>(sizeof(rounds)));
        TAP_CHECK(other.join(PW_JOIN_US) == 0);
        TAP_CHECK(pw_recv(ep, &rounds, sizeof(rounds), 20000) == -KOS_ETIMEDOUT);
        kos_sem_post(sa);
        PwSeen seen = {-99, -99, -99, -1, -1};
        TAP_CHECK(pw_recv(ep, &seen, sizeof(seen), PW_JOIN_US)
                  == static_cast<int32_t>(sizeof(seen)));
        TAP_CHECK(holder.join(PW_JOIN_US) == 0);
        TAP_CHECK(rounds == PW_ROUNDS);
        TAP_CHECK(seen.index == 0 and seen.nmi == -KOS_EINVAL and seen.unheld == -KOS_EPERM);
        TAP_CHECK(seen.rounds == PW_ROUNDS + 1 and seen.moved == 0);
        // The CMOS pair is free again, and its index stays closed to the next holder.
        kos_cap_grant const icaps[] = {{ep, KOS_CAP_SIGNAL}};
        auto const writer = kos::thread::create(pw_index, nullptr, "pwi", 10, KOS_POLICY_FIFO,
                                                0, false, nullptr, 0, nullptr, 0, &cmos, 1,
                                                icaps, 1, 0, nullptr, ti);
        TAP_CHECK(writer.valid());
        if (writer.valid())
        {
            char x = 0;
            TAP_CHECK(pw_recv(ep, &x, 1, 20000) == -KOS_ETIMEDOUT);
            TAP_CHECK(writer.join(PW_JOIN_US) == 0);
        }
        // A task of its own: an ended task takes no member, and ti's last one has exited.
        kos_task_t tk = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &tk) == 0);
        kos_window const com1 = {0x3f8u, 8u, KOS_WINDOW_PORTS, 0};
        auto const console = kos::thread::create(pw_noop, nullptr, "pwk", 10, KOS_POLICY_FIFO, 0,
                                                 false, nullptr, 0, nullptr, 0, &com1, 1, nullptr,
                                                 0, 0, nullptr, tk);
#if defined(KICKOS_SELFTEST_CONSOLE_DRIVER)
        TAP_CHECK(console.error() == -KOS_EBUSY);
#else
        TAP_CHECK(console.error() == 0);
        TAP_CHECK(console.join(PW_JOIN_US) == 0);
#endif
        (void)kos_task_kill(tk);
        (void)kos_task_kill(tc);
        (void)kos_task_kill(tb);
        (void)kos_task_kill(ti);
        (void)kos_handle_close(sa);
        (void)kos_handle_close(sb);
        (void)kos_handle_close(ep);
    }
#endif

#if defined(KICKOS_SELFTEST_SPARE_DEV)
    // --- A device window at the address the kernel chose -------------------------------
    // The holder, in a task of its own, reads a device the kernel never drives through the
    // address kos_window_get answers, which is not the physical one, and is answered no window
    // past its list; it reports over an endpoint, its task's data being its own.
    // While it lives a second task is refused the device; once it has exited, its sibling in
    // the task faults on the address it left, and the next instance is granted the device and
    // reaches it again.
    constexpr uintptr_t WA_DEV = KICKOS_SELFTEST_SPARE_DEV;
    constexpr uint32_t WA_SIZE = 0x1000u;
    constexpr uint32_t WA_JOIN_US = 500000;
    struct WaSeen
    {
        int32_t rc;
        int32_t other;
        uint32_t value;
        uint32_t moved;
    };
    uintptr_t g_wa_addr = 0; // the holder's task's copy, which its sibling reads
    void wa_holder(void*) // caps: E(SIGNAL)@1, hold@2
    {
        kos_window at = {};
        WaSeen seen = {-99, -99, 0, 0};
        seen.rc = kos_window_get(0, &at);
        g_wa_addr = at.base;
        if (seen.rc == 0)
        {
            if (g_wa_addr != WA_DEV)
            {
                seen.moved = 1u;
            }
            seen.value = *reinterpret_cast<volatile uint32_t*>(at.base);
        }
        kos_window other = {};
        seen.other = kos_window_get(1, &other);
        (void)kos_send(1, &seen, sizeof(seen));
        kos_sem_wait(2);
        kos_exit(0);
    }
    void wa_sibling(void*) // caps: go@1, E(SIGNAL)@2
    {
        kos_sem_wait(1);
        (void)*reinterpret_cast<volatile uint32_t*>(g_wa_addr);
        (void)kos_send(2, "x", 1); // unreachable: the holder's exit took the mapping with it
        kos_exit(0);
    }
    void wa_noop(void*) {}
#if defined(KICKOS_SELFTEST_BUS_MASTER_DEV)
    struct WaBusMaster
    {
        int32_t spare;
        int32_t bus_master;
    };
    // [0] holds AUTH_MEMORY, AUTH_TASKS and AUTH_SYSTEM; [1] AUTH_MEMORY, AUTH_TASKS and
    // AUTH_BUS_MASTER.
    WaBusMaster g_wa_bm[2] = {{-99, -99}, {-99, -99}};
    kos_window const g_wa_bm_dev = {KICKOS_SELFTEST_BUS_MASTER_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
    void wa_bus_master_spawner(void* arg) // caps: done@1
    {
        WaBusMaster* const out = static_cast<WaBusMaster*>(arg);
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) == 0)
        {
            kos_window const spare = {WA_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
            auto const h = kos::thread::create(wa_noop, nullptr, "wbs", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0, &spare, 1,
                                               nullptr, 0, 0, nullptr, t);
            out->spare = h.error();
            (void)h.join(WA_JOIN_US);
            (void)kos_task_kill(t);
        }
        // A task of its own: the one above ended with its entry.
        if (kos_task_create(nullptr, 0, 0, &t) == 0)
        {
            auto const b = kos::thread::create(wa_noop, nullptr, "wbm", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0, &g_wa_bm_dev, 1,
                                               nullptr, 0, 0, nullptr, t);
            out->bus_master = b.error();
            (void)b.join(WA_JOIN_US);
            (void)kos_task_kill(t);
        }
        kos_sem_post(CH_DONE);
    }
#endif
    // Spawns a holder of the device in `task` and receives its report.
    kos::thread::Handle wa_spawn(kos_task_t task, kos_cap_t ep, kos_cap_t hold, WaSeen* seen)
    {
        kos_window const dev = {WA_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
        kos_cap_grant const caps[] = {{ep, KOS_CAP_SIGNAL}, {hold, KOS_CAP_WAIT}};
        *seen = {-99, -99, 0, 0};
        auto h = kos::thread::create(wa_holder, nullptr, "wah", 10, KOS_POLICY_FIFO, 0, false,
                                     nullptr, 0, nullptr, 0, &dev, 1, caps, 2, 0, nullptr, task);
        if (h.valid())
        {
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, WA_JOIN_US);
            (void)kos_reply_recv(KOS_CAP_NONE, seen, kos_call_lens_pack(0, sizeof(*seen)), &o);
        }
        return h;
    }
    void t_window_addr()
    {
        settle_exits();
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t hold = KOS_CAP_NONE;
        kos_cap_t go = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        kos_task_t other = KOS_TASK_NONE;
        if (kos_endpoint_create(&ep) != 0 or kos_sem_create(0, &hold) != 0
            or kos_sem_create(0, &go) != 0 or kos_task_create(nullptr, 0, 0, &t) != 0
            or kos_task_create(nullptr, 0, 0, &other) != 0)
        {
            tap::skip("endpoint, semaphore or task pool too small");
            return;
        }
        WaSeen first = {-99, -99, 0, 0};
        auto const holder = wa_spawn(t, ep, hold, &first);
        kos_cap_grant const scaps[] = {{go, KOS_CAP_WAIT}, {ep, KOS_CAP_SIGNAL}};
        auto const sibling = kos::thread::create(wa_sibling, nullptr, "was", 10,
                                                 KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr,
                                                 0, nullptr, 0, scaps, 2, 0, nullptr, t);
        TAP_CHECK(holder.valid() and sibling.valid());
        kos_window const dev = {WA_DEV, WA_SIZE, KOS_WINDOW_DEVICE, 0};
        TAP_CHECK(kos::thread::create(wa_noop, nullptr, "wab", 10, KOS_POLICY_FIFO, 0, false,
                                      nullptr, 0, nullptr, 0, &dev, 1, nullptr, 0, 0, nullptr,
                                      other)
                      .error()
                  == -KOS_EBUSY);
        // A megabyte past the spare device is outside every aperture this chip states.
        kos_window const outside = {WA_DEV + 0x100000u, WA_SIZE, KOS_WINDOW_DEVICE, 0};
        TAP_CHECK(kos::thread::create(wa_noop, nullptr, "wao", 10, KOS_POLICY_FIFO, 0, false,
                                      nullptr, 0, nullptr, 0, &outside, 1, nullptr, 0, 0,
                                      nullptr, other)
                      .error()
                  == -KOS_EINVAL);
        kos_sem_post(hold);
        TAP_CHECK(holder.join(WA_JOIN_US) == 0);
        kos_sem_post(go);
        TAP_CHECK(sibling.join(WA_JOIN_US) == 0);
        char late = 0;
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, 20000);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, &late, kos_call_lens_pack(0, 1), &o)
                  == -KOS_ETIMEDOUT);
        (void)kos_task_kill(t);
        TAP_CHECK(first.rc == 0 and first.moved == 1u and first.value != 0);
        TAP_CHECK(first.other == -KOS_EINVAL);
        // The next instance, in the other task, maps the device again.
        WaSeen again = {-99, -99, 0, 0};
        kos_sem_post(hold);
        auto const next = wa_spawn(other, ep, hold, &again);
        TAP_CHECK(next.valid() and next.join(WA_JOIN_US) == 0);
        TAP_CHECK(again.rc == 0 and again.value != 0);
#if defined(KICKOS_SELFTEST_BUS_MASTER_DEV)
        // A bus master's window takes AUTH_BUS_MASTER of its spawner; AUTH_SYSTEM is not it.
        kos_task_t bt = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &bt) == 0);
        auto const bm = kos::thread::create(wa_noop, nullptr, "wbm", 10, KOS_POLICY_FIFO, 0, false,
                                            nullptr, 0, nullptr, 0, &g_wa_bm_dev, 1, nullptr, 0,
                                            0, nullptr, bt);
        TAP_CHECK(bm.valid() and bm.join(WA_JOIN_US) == 0);
        (void)kos_task_kill(bt);
        uint32_t const bauth[2] = {KOS_AUTH_MEMORY | KOS_AUTH_TASKS | KOS_AUTH_SYSTEM,
                                   KOS_AUTH_MEMORY | KOS_AUTH_TASKS | KOS_AUTH_BUS_MASTER};
        for (int i = 0; i < 2; i++)
        {
            g_wa_bm[i] = {-99, -99};
            kos_cap_grant const bcaps[] = {{g_done, CH_FULL}};
            auto const spawner = kos::thread::create_caps(wa_bus_master_spawner, &g_wa_bm[i],
                                                          "wbp", 10, bcaps, 1, KOS_POLICY_FIFO,
                                                          0, false, nullptr, 0, bauth[i]);
            TAP_CHECK(spawner.valid());
            if (spawner.valid())
            {
                wait_n(1);
                (void)spawner.join();
            }
        }
        tap::diag("bus master window: memory and system %ld, bus_master %ld",
                  static_cast<long>(g_wa_bm[0].bus_master),
                  static_cast<long>(g_wa_bm[1].bus_master));
        TAP_CHECK(g_wa_bm[0].spare == 0 and g_wa_bm[1].spare == 0);
        TAP_CHECK(g_wa_bm[0].bus_master == -KOS_EPERM);
        TAP_CHECK(g_wa_bm[1].bus_master == 0);
#endif
        (void)kos_task_kill(other);
        (void)kos_handle_close(hold);
        (void)kos_handle_close(go);
        (void)kos_handle_close(ep);
    }
#endif
#endif

    // main has memory authority but must not grant an unreserved kernel address. Do not reject
    // all high addresses: the user arena is high-half on this board.
    void t_grant_kernel_word_refused()
    {
        void* const kword = kos_guard_addr();
        if (kword == nullptr)
        {
            tap::skip("this board names no privileged-only word");
            return;
        }
        void* const mine = kos_ram_alloc(64);
        if (mine == nullptr)
        {
            tap::skip("no reservation left for the positive control");
            return;
        }
        tap::diag("self-granting the kernel word 0x%lx against own range 0x%lx",
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(kword)),
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(mine)));
        TAP_CHECK(kos_mem_self_grant(kword, sizeof(uint32_t), 0) == -KOS_EPERM);
        // Control: a caller-owned reservation, same syscall.
        TAP_CHECK(kos_mem_self_grant(mine, 64, 0) == 0);
        // Still refused after a success on the same path, so the refusal is not a one-shot
        // state the first call left behind.
        TAP_CHECK(kos_mem_self_grant(kword, sizeof(uint32_t), 0) == -KOS_EPERM);
    }

    // Re-grants must honor memory-type changes in both directions, even when access rights
    // already match.
    void t_self_grant_retype()
    {
        settle_exits();
        constexpr uint32_t RT_BLK = 256;
        // 1 + the enum value, which is what KOS_ASPACE_OP_MEMTYPE_AT answers.
        constexpr uint64_t RT_NORMAL_AT = 1u;
        constexpr uint64_t RT_NOCACHE_AT = 2u;
        if (kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 1) == 0)
        {
            tap::skip("this backend honours no non-cacheable type");
            return;
        }
        void* const blk = kos_ram_alloc(RT_BLK);
        if (blk == nullptr)
        {
            tap::skip("no reservation left for the re-type block");
            return;
        }
        uintptr_t const at = reinterpret_cast<uintptr_t>(blk);
        TAP_CHECK(kos_mem_self_grant(blk, RT_BLK, 0) == 0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, at) == RT_NORMAL_AT);
        TAP_CHECK(kos_mem_self_grant(blk, RT_BLK, KOS_MEM_NOCACHE) == 0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, at) == RT_NOCACHE_AT);
        TAP_CHECK(kos_mem_self_grant(blk, RT_BLK, 0) == 0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, at) == RT_NORMAL_AT);
        // Idempotent in the state it landed in, so the leg above is not a one-shot.
        TAP_CHECK(kos_mem_self_grant(blk, RT_BLK, 0) == 0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, at) == RT_NORMAL_AT);
        // Still reachable, which a re-map that broke the mapping down and failed would lose.
        volatile uint32_t* const w = static_cast<volatile uint32_t*>(blk);
        w[0] = 0xA5A5u;
        TAP_CHECK(w[0] == 0xA5A5u);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // The kernel's maintenance of its cacheable view of a non-cacheable block, counted for a map
    // and for IPC into and out of it; a cacheable block costs none.
#if defined(__riscv)
    constexpr uint64_t UA_PER = 0; // the PMAs type a frame for the kernel's view too
#else
    constexpr uint64_t UA_PER = 1;
#endif
    constexpr size_t UA_LEN = 16;
    constexpr uint32_t UA_BLK = 128;
    constexpr int UA_ROUNDS = 3;
    constexpr uint32_t UA_US = 500000;
    constexpr int CH_UA_EP = 2;
    unsigned char* g_ua_dst[UA_ROUNDS] = {};
    Atomic<int32_t, Order::RELAXED> g_ua_got[UA_ROUNDS];
    void ua_receiver(void*) // caps: done@1, E(WAIT)@2
    {
        for (int r = 0; r < UA_ROUNDS; r++)
        {
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, CH_UA_EP, KOS_RECV_NO_INFO, UA_US);
            g_ua_got[r] = kos_reply_recv(KOS_CAP_NONE, g_ua_dst[r],
                                         kos_call_lens_pack(0, UA_LEN), &o);
            kos_sem_post(CH_DONE);
        }
    }
    uint64_t ua_syncs()
    {
        return kos_aspace_probe(KOS_ASPACE_OP_ALIAS_SYNCS, 0);
    }
    uint64_t pr_refusals()
    {
        return kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_REFUSALS, 0);
    }
    uint64_t pr_granules()
    {
        return kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_GRANULES, 0) & 0xFFFFFFFFull;
    }
    uint64_t pr_staged()
    {
        return kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_GRANULES, 0) >> 32;
    }
    uint64_t pr_windows()
    {
        return kos_aspace_probe(KOS_ASPACE_OP_IRQ_WINDOWS, 0);
    }
    constexpr uint64_t UA_WIDE_PAGES = 32;
    void ua_noop(void*) {}
    void* g_ua_unauth_blk = nullptr;
    int32_t g_ua_unauth = 0;
    uint64_t g_ua_unauth_syncs = 0;
    void ua_unauthorised(void*)
    {
        uint64_t const before = kos_aspace_probe(KOS_ASPACE_OP_ALIAS_SYNCS, 0);
        g_ua_unauth = kos_mem_self_grant(g_ua_unauth_blk, UA_BLK, KOS_MEM_NOCACHE);
        g_ua_unauth_syncs = kos_aspace_probe(KOS_ASPACE_OP_ALIAS_SYNCS, 0) - before;
        kos_sem_post(CH_DONE);
    }
    // A wide non-cacheable block self-granted, mapped as a child's window and handed to a task
    // as its data, each sync made ahead of the kernel lock with an interrupt window per granule.
    void ua_wide()
    {
        uint64_t const g = static_cast<uint64_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        uint32_t const bytes = static_cast<uint32_t>(UA_WIDE_PAGES * g);
        void* const wide = kos_ram_alloc(bytes);
        if (g == 0 or wide == nullptr)
        {
            tap::skip("no reservation left for the wide block");
            return;
        }
        uint64_t const r0 = pr_refusals();
        uint64_t const g0 = pr_granules();
        uint64_t const st0 = pr_staged();
        uint64_t const w0 = pr_windows();
        uint64_t const s0 = ua_syncs();
        TAP_CHECK(kos_mem_self_grant(wide, bytes, KOS_MEM_NOCACHE) == 0);
        uint64_t const s1 = ua_syncs();
        kos_window const win = {reinterpret_cast<uintptr_t>(wide), bytes, KOS_WINDOW_MEMORY,
                                KOS_WINDOW_UNCACHED};
        kos_task_t t = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        auto const child = kos::thread::create(ua_noop, nullptr, "uaw", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0, &win, 1, nullptr, 0,
                                               0, nullptr, t);
        TAP_CHECK(child.valid() and child.join(UA_US) == 0);
        (void)kos_task_kill(t);
        uint64_t const s2 = ua_syncs();
        kos_task_t data = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(wide, bytes, KOS_MEM_NOCACHE, &data) == 0);
        (void)kos_task_kill(data);
        uint64_t const s3 = ua_syncs();
        uint64_t const windows = pr_windows() - w0;
        uint64_t const masked = kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_MASKED, 0);
        tap::diag("alias syncs over %lu granules: self-grant %lu, window %lu, task data %lu",
                  static_cast<unsigned long>(UA_WIDE_PAGES), static_cast<unsigned long>(s1 - s0),
                  static_cast<unsigned long>(s2 - s1), static_cast<unsigned long>(s3 - s2));
        tap::diag("ahead of the lock: %lu granules, %lu interrupt windows, %lu rounds sent back;"
                  " at most %lu granules and %lu ns between two windows",
                  static_cast<unsigned long>(pr_granules() - g0),
                  static_cast<unsigned long>(windows),
                  static_cast<unsigned long>(pr_refusals() - r0),
                  static_cast<unsigned long>(masked & 0xFFFFFFFFull),
                  static_cast<unsigned long>(masked >> 32));
        TAP_CHECK(s1 - s0 == UA_WIDE_PAGES * UA_PER);
        TAP_CHECK(s2 - s1 == UA_WIDE_PAGES * UA_PER);
        TAP_CHECK(s3 - s2 == UA_WIDE_PAGES * UA_PER);
        TAP_CHECK(pr_granules() - g0 == 3u * UA_WIDE_PAGES * UA_PER);
        TAP_CHECK(pr_refusals() == r0);
        // Counted where the arch opens each window, not by the loop that asks for one.
        TAP_CHECK(windows >= (pr_granules() - g0) + (pr_staged() - st0));
    }
    void t_uncached_alias_sync()
    {
        settle_exits();
        if (kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 1) == 0)
        {
            tap::skip("this backend honours no non-cacheable type");
            return;
        }
        unsigned char* const unc = static_cast<unsigned char*>(kos_ram_alloc(UA_BLK));
        unsigned char* const cac = static_cast<unsigned char*>(kos_ram_alloc(UA_BLK));
        kos_cap_t ep = KOS_CAP_NONE;
        if (unc == nullptr or cac == nullptr or kos_endpoint_create(&ep) != 0)
        {
            tap::skip("no reservation or endpoint left for the pair");
            return;
        }
        uint64_t const at_cac = ua_syncs();
        TAP_CHECK(kos_mem_self_grant(cac, UA_BLK, 0) == 0);
        uint64_t const at_unc = ua_syncs();
        TAP_CHECK(kos_mem_self_grant(unc, UA_BLK, KOS_MEM_NOCACHE) == 0);
        uint64_t const mapped = ua_syncs();
        tap::diag("alias syncs: cacheable map %lu, non-cacheable map %lu",
                  static_cast<unsigned long>(at_unc - at_cac),
                  static_cast<unsigned long>(mapped - at_unc));
        TAP_CHECK(at_unc - at_cac == 0);
        TAP_CHECK(mapped - at_unc == UA_PER);
        // Refused, or mapped so already: the call answers before it syncs anything.
        g_ua_unauth_blk = cac;
        g_ua_unauth = 99;
        kos_cap_grant const ucaps[] = {{g_done, CH_FULL}};
        if (kos::thread::create_caps(ua_unauthorised, nullptr, "uanA", 10, ucaps, 1,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0)
                .valid())
        {
            wait_n(1);
            tap::diag("a non-cacheable grant with no memory authority: %ld, %lu alias syncs",
                      static_cast<long>(g_ua_unauth),
                      static_cast<unsigned long>(g_ua_unauth_syncs));
            TAP_CHECK(g_ua_unauth == -KOS_EPERM);
            TAP_CHECK(g_ua_unauth_syncs == 0);
        }
        uint64_t const pr_at = kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_GRANULES, 0);
        TAP_CHECK(kos_mem_self_grant(unc, UA_BLK, KOS_MEM_NOCACHE) == 0);
        uint64_t const pr_again = kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_GRANULES, 0) - pr_at;
        uint64_t const again = ua_syncs() - mapped;
        tap::diag("the same grant again: %lu alias syncs, %lu granules ahead of the lock",
                  static_cast<unsigned long>(again), static_cast<unsigned long>(pr_again));
        TAP_CHECK(again == 0);
        TAP_CHECK(UA_PER == 0 or pr_again == 0);
        for (size_t i = 0; i < UA_LEN; i++)
        {
            cac[UA_LEN * 2 + i] = static_cast<unsigned char>(0x40u + i);
            unc[UA_LEN * 2 + i] = static_cast<unsigned char>(0x80u + i);
        }
        unsigned char const* const src[UA_ROUNDS] = {cac + UA_LEN * 2, unc + UA_LEN * 2,
                                                     cac + UA_LEN * 2};
        g_ua_dst[0] = unc;
        g_ua_dst[1] = cac;
        g_ua_dst[2] = cac + UA_LEN * 4;
        uint64_t const want[UA_ROUNDS] = {2u * UA_PER, UA_PER, 0u};
        for (int r = 0; r < UA_ROUNDS; r++)
        {
            g_ua_got[r] = 99;
        }
        kos_cap_grant const caps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_WAIT}};
        if (not kos::thread::create_caps(ua_receiver, nullptr, "uarcv", TAP_PRIO_PARKS, caps, 2,
                                         KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                         KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE)
                    .valid())
        {
            (void)kos_handle_close(ep);
            tap::skip("thread pool too small for the receiver");
            return;
        }
        for (int r = 0; r < UA_ROUNDS; r++)
        {
            uint64_t const before = ua_syncs();
            int32_t const sent = kos_send_timed(ep, src[r], UA_LEN, UA_US);
            wait_n(1);
            uint64_t const copied = ua_syncs() - before;
            tap::diag("round %d: sent %ld, received %ld, alias syncs %lu", r,
                      static_cast<long>(sent), static_cast<long>(g_ua_got[r].load()),
                      static_cast<unsigned long>(copied));
            TAP_CHECK(sent == static_cast<int32_t>(UA_LEN));
            TAP_CHECK(g_ua_got[r].load() == static_cast<int32_t>(UA_LEN));
            TAP_CHECK(copied == want[r]);
            bool same = true;
            for (size_t i = 0; i < UA_LEN; i++)
            {
                if (g_ua_dst[r][i] != src[r][i])
                {
                    same = false;
                }
            }
            TAP_CHECK(same);
        }
        settle_exits();
        (void)kos_handle_close(ep);
        // Back to cacheable: the lines the kernel's reads left are stale to the new mapping.
        uint64_t const at_back = ua_syncs();
        TAP_CHECK(kos_mem_self_grant(unc, UA_BLK, 0) == 0);
        uint64_t const back = ua_syncs() - at_back;
        tap::diag("alias syncs: retype to cacheable %lu", static_cast<unsigned long>(back));
        TAP_CHECK(back == UA_PER);
        // A frame capability's run outlives its non-cacheable leaf: the unmap syncs nothing, and
        // the next cacheable mapping of the run owes the sync, once.
        uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0);
        uintptr_t const fva = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 0);
        TAP_CHECK(seed != 0 and fva != 0);
        if (seed != 0 and fva != 0)
        {
            kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFull);
            kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
            uintptr_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
            uint64_t const c0 = ua_syncs();
            TAP_CHECK(kos_frame_map(fcap, acap, fva, KOS_MEM_NOCACHE) == 0);
            uint64_t const c1 = ua_syncs();
            // One run, two types at once, is incoherent.
            int32_t const mixed = kos_frame_map(fcap, acap, fva + 4u * g, 0);
            TAP_CHECK(mixed == -KOS_EBUSY);
            if (mixed == 0)
            {
                (void)kos_frame_unmap(fcap, acap, fva + 4u * g);
            }
            TAP_CHECK(kos_frame_unmap(fcap, acap, fva) == 0);
            uint64_t const c2 = ua_syncs();
            TAP_CHECK(kos_frame_map(fcap, acap, fva, 0) == 0);
            uint64_t const c3 = ua_syncs();
            TAP_CHECK(kos_frame_unmap(fcap, acap, fva) == 0);
            TAP_CHECK(kos_frame_map(fcap, acap, fva, 0) == 0);
            uint64_t const c4 = ua_syncs();
            TAP_CHECK(kos_frame_unmap(fcap, acap, fva) == 0);
            (void)kos_handle_close(fcap);
            (void)kos_handle_close(acap);
            tap::diag("alias syncs: frame map %lu, unmap %lu, cacheable map %lu, again %lu",
                      static_cast<unsigned long>(c1 - c0), static_cast<unsigned long>(c2 - c1),
                      static_cast<unsigned long>(c3 - c2), static_cast<unsigned long>(c4 - c3));
            TAP_CHECK(c1 - c0 == UA_PER);
            TAP_CHECK(c2 - c1 == 0);
            TAP_CHECK(c3 - c2 == UA_PER);
            TAP_CHECK(c4 - c3 == 0);
        }
        ua_wide();
    }

    // A non-cacheable mapping torn down without a sync of its own: a spawn window at its
    // holder's exit, a task's data at the task's end. The next cacheable mapping of the frames
    // owes the sync the kernel's reads through its cacheable view left.
    void td_holder(void*) // caps: gate@1
    {
        kos_sem_wait(1);
    }
    constexpr uint64_t TD_PAGES = 3;
    void t_uncached_teardown()
    {
        settle_exits();
        if (kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 1) == 0)
        {
            tap::skip("this backend honours no non-cacheable type");
            return;
        }
        uint64_t const g = static_cast<uint64_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        uint32_t const bytes = static_cast<uint32_t>(TD_PAGES * g);
        void* const held = kos_ram_alloc(bytes);
        void* const data = kos_ram_alloc(bytes);
        kos_task_t t = KOS_TASK_NONE;
        kos_task_t later = KOS_TASK_NONE;
        if (g == 0 or held == nullptr or data == nullptr or kos_task_create(nullptr, 0, 0, &t) != 0
            or kos_task_create(nullptr, 0, 0, &later) != 0)
        {
            tap::skip("no reservation or task left for the teardown pair");
            return;
        }
        kos_cap_t gate = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(0, &gate) == 0);
        kos_window const win = {reinterpret_cast<uintptr_t>(held), bytes, KOS_WINDOW_MEMORY,
                                KOS_WINDOW_UNCACHED};
        kos_cap_grant const caps[] = {{gate, KOS_CAP_WAIT}};
        // Held until the count below is taken, so the teardown falls inside it.
        auto const child = kos::thread::create(td_holder, nullptr, "utd", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0, &win, 1, caps, 1,
                                               0, nullptr, t);
        kos_task_t d = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(data, bytes, KOS_MEM_NOCACHE, &d) == 0);
        uint64_t const before = ua_syncs();
        kos_sem_post(gate);
        TAP_CHECK(child.valid() and child.join(UA_US) == 0);
        (void)kos_task_kill(t);
        (void)kos_task_kill(d);
        settle_exits();
        uint64_t const s0 = ua_syncs();
        int32_t const w = kos_mem_self_grant(held, bytes, 0);
        uint64_t const s1 = ua_syncs();
        int32_t const h = kos_mem_self_grant(data, bytes, 0);
        uint64_t const s2 = ua_syncs();
        kos_window const plain = {reinterpret_cast<uintptr_t>(held), bytes, KOS_WINDOW_MEMORY, 0};
        auto const second = kos::thread::create(ua_noop, nullptr, "utc", 10, KOS_POLICY_FIFO, 0,
                                                false, nullptr, 0, nullptr, 0, &plain, 1, nullptr,
                                                0, 0, nullptr, later);
        uint64_t const s2b = ua_syncs();
        TAP_CHECK(second.valid() and second.join(UA_US) == 0);
        (void)kos_task_kill(later);
        int32_t const again = kos_mem_self_grant(held, bytes, KOS_MEM_NOCACHE);
        int32_t const back = kos_mem_self_grant(held, bytes, 0);
        uint64_t const s3 = ua_syncs();
        (void)kos_handle_close(gate);
        tap::diag("across the teardowns %lu; cacheable grants after them: window %ld syncing %lu,"
                  " task data %ld syncing %lu, a second cacheable mapping %lu; a retype round trip"
                  " %ld %ld syncing %lu",
                  static_cast<unsigned long>(s0 - before), static_cast<long>(w),
                  static_cast<unsigned long>(s1 - s0), static_cast<long>(h),
                  static_cast<unsigned long>(s2 - s1), static_cast<unsigned long>(s2b - s2),
                  static_cast<long>(again), static_cast<long>(back),
                  static_cast<unsigned long>(s3 - s2b));
        TAP_CHECK(s0 - before == 0);
        TAP_CHECK(w == 0 and h == 0 and again == 0 and back == 0);
        TAP_CHECK(s1 - s0 == TD_PAGES * UA_PER);
        TAP_CHECK(s2 - s1 == TD_PAGES * UA_PER);
        TAP_CHECK(s2b - s2 == 0);
        TAP_CHECK(s3 - s2b == 2u * TD_PAGES * UA_PER);
    }

    // Each call that syncs ahead of its lock, its record lost on its first PR_DROPS rounds as
    // to another call's completed edit: the call syncs again each round and then succeeds.
    constexpr uint32_t PR_PAGES = 20;
    constexpr uint64_t PR_DROPS_SHIFT = 16;
    constexpr uint64_t PR_DROPS = 3;
    constexpr uint64_t PR_DROPS_FOREVER = 0xFF;
    constexpr uint64_t PR_FAULT_OUT = 1ull << 24;
    uint64_t pr_own_windows()
    {
        return kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_WINDOWS, 0);
    }
    void pr_drop_next(uint64_t drops)
    {
        (void)kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_ARM, drops << PR_DROPS_SHIFT);
    }
    uint64_t pr_threads_live()
    {
        return kos_aspace_probe(KOS_ASPACE_OP_THREADS_LIVE, 0);
    }
    void t_presync_retried()
    {
        settle_exits();
        if (kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 1) == 0 or UA_PER == 0)
        {
            tap::skip("no cacheable kernel view of a non-cacheable frame here");
            return;
        }
        uint64_t const g = static_cast<uint64_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        uint32_t const bytes = static_cast<uint32_t>(PR_PAGES * g);
        uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, PR_PAGES);
        uintptr_t const va = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, PR_PAGES);
        // One block for three calls: a non-cacheable handoff and window sync whatever the
        // block's live mappings, as long as they are non-cacheable too.
        void* const blk = kos_ram_alloc(bytes);
        kos_task_t into = KOS_TASK_NONE;
        if (seed == 0 or va == 0 or blk == nullptr or kos_task_create(nullptr, 0, 0, &into) != 0)
        {
            tap::skip("no frame run, reservation or task left for the four calls");
            return;
        }
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFull);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        uint64_t const live0 = pr_threads_live();
        uint64_t r[5] = {};
        uint64_t const g0 = pr_granules();
        r[0] = pr_refusals();
        pr_drop_next(PR_DROPS);
        int32_t const mapped = kos_frame_map(fcap, acap, va, KOS_MEM_NOCACHE);
        r[1] = pr_refusals();
        uint64_t const g1 = pr_granules();
        int32_t const unmapped = kos_frame_unmap(fcap, acap, va);
        pr_drop_next(PR_DROPS);
        int32_t const granted = kos_mem_self_grant(blk, bytes, KOS_MEM_NOCACHE);
        r[2] = pr_refusals();
        kos_task_t data = KOS_TASK_NONE;
        pr_drop_next(PR_DROPS);
        int32_t const created = kos_task_create(blk, bytes, KOS_MEM_NOCACHE, &data);
        r[3] = pr_refusals();
        kos_window const win = {reinterpret_cast<uintptr_t>(blk), bytes, KOS_WINDOW_MEMORY,
                                KOS_WINDOW_UNCACHED};
        pr_drop_next(PR_DROPS);
        auto const child = kos::thread::create(ua_noop, nullptr, "prc", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0, &win, 1, nullptr, 0,
                                               0, nullptr, into);
        r[4] = pr_refusals();
        bool const joined = child.valid() and child.join(UA_US) == 0;
        (void)kos_task_kill(into);
        (void)kos_task_kill(data);
        (void)kos_handle_close(fcap);
        (void)kos_handle_close(acap);
        settle_exits();
        tap::diag("rounds sent back over %lu dropped records: frame map %ld after %lu, self-grant"
                  " %ld after %lu, task data %ld after %lu, window %d after %lu; %lu granules",
                  static_cast<unsigned long>(PR_DROPS), static_cast<long>(mapped),
                  static_cast<unsigned long>(r[1] - r[0]), static_cast<long>(granted),
                  static_cast<unsigned long>(r[2] - r[1]), static_cast<long>(created),
                  static_cast<unsigned long>(r[3] - r[2]), child.error(),
                  static_cast<unsigned long>(r[4] - r[3]),
                  static_cast<unsigned long>(g1 - g0));
        TAP_CHECK(mapped == 0 and unmapped == 0 and r[1] - r[0] == PR_DROPS);
        TAP_CHECK(g1 - g0 == (PR_DROPS + 1u) * PR_PAGES);
        TAP_CHECK(granted == 0 and r[2] - r[1] == PR_DROPS);
        TAP_CHECK(created == 0 and data != KOS_TASK_NONE and r[3] - r[2] == PR_DROPS);
        TAP_CHECK(joined and r[4] - r[3] == PR_DROPS);
        TAP_CHECK(pr_threads_live() == live0);
    }

    // An interrupt pending when a call starts its work outside the lock is taken inside it, and
    // wakes a thread that acts while the call is still in its system call. A mapping that thread
    // completes over the call's frames sends the call round again, its handle lost or not; a
    // failed one, an unmap, a close or a read does not.
#if defined(KICKOS_IRQ_SOFT_ONLY_BASE)
    constexpr int PR_LINE = KICKOS_IRQ_SOFT_ONLY_BASE + 2;
#else
    constexpr int PR_LINE = KICKOS_IRQ_FREE_BASE + 12;
#endif
    constexpr int CH_PR_NOTE = 2;
    constexpr int CH_PR_FRAME = 3;
    constexpr int CH_PR_SPACE = 4;
    constexpr uint8_t PR_PRIO = 20;
    enum PrAction
    {
        PR_CAP_MAP,
        PR_SELF_GRANT,
        PR_WINDOW,
        PR_HANDOFF,
        PR_FAULTED_OUT,
        PR_FAILED_SPAWN,
        PR_UNMAP,
        PR_CLOSE,
        PR_FLIP,
        PR_WATCH,
        PR_LATE_PARAMS,
        PR_ACTIONS
    };
    struct PrEdit
    {
        int action;
        void* blk;
        uint32_t bytes;
        int32_t woke;
        int32_t rc;
        uint64_t main_live;
        uint64_t main_live_after;
        int in_call;
    };
    PrEdit g_pr = {};
    uintptr_t g_pr_va = 0;
    uintptr_t g_pr_va2 = 0;
    kos_thread_t g_pr_main = KOS_THREAD_NONE;
    volatile int g_pr_in_call = 0;
    kos_thread_params g_flip = {};
    void const* g_pl_src = nullptr;
    size_t g_pl_len = 0;
    // Before anything that can block: main runs again on this core once this thread waits, and
    // its next round re-arms the record.
    void pr_edited()
    {
        g_pr.main_live_after = kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_LIVE, g_pr_main);
    }
    void pr_edit()
    {
        kos_window const win = {reinterpret_cast<uintptr_t>(g_pr.blk), g_pr.bytes,
                                KOS_WINDOW_MEMORY, KOS_WINDOW_UNCACHED};
        kos_task_t t = KOS_TASK_NONE;
        switch (g_pr.action)
        {
            case PR_CAP_MAP:
            {
                g_pr.rc = kos_frame_map(CH_PR_FRAME, CH_PR_SPACE, g_pr_va2, 0);
                pr_edited();
                (void)kos_frame_unmap(CH_PR_FRAME, CH_PR_SPACE, g_pr_va2);
                break;
            }
            case PR_SELF_GRANT:
            {
                g_pr.rc = kos_mem_self_grant(g_pr.blk, g_pr.bytes, KOS_MEM_NOCACHE);
                pr_edited();
                break;
            }
            case PR_WINDOW:
            case PR_FAULTED_OUT:
            case PR_FAILED_SPAWN:
            {
                if (kos_task_create(nullptr, 0, 0, &t) != 0)
                {
                    break;
                }
                // A destination past the child's table fails the spawn after its window is
                // mapped.
                kos_cap_grant const caps[] = {{CH_DONE, KOS_CAP_SIGNAL}};
                uint16_t const dest[] = {0x7FFFu};
                uint8_t ncaps = 0;
                uint16_t const* at = nullptr;
                if (g_pr.action == PR_FAILED_SPAWN)
                {
                    ncaps = 1;
                    at = dest;
                }
                if (g_pr.action == PR_FAULTED_OUT)
                {
                    (void)kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_ARM, PR_FAULT_OUT);
                }
                auto const c = kos::thread::create(ua_noop, nullptr, "pre", 10, KOS_POLICY_FIFO,
                                                   0, false, nullptr, 0, nullptr, 0, &win, 1,
                                                   caps, ncaps, 0, at, t);
                g_pr.rc = c.error();
                pr_edited();
                if (c.valid())
                {
                    (void)c.join(UA_US);
                }
                (void)kos_task_kill(t);
                break;
            }
            case PR_HANDOFF:
            {
                g_pr.rc = kos_task_create(g_pr.blk, g_pr.bytes, KOS_MEM_NOCACHE, &t);
                pr_edited();
                (void)kos_task_kill(t);
                break;
            }
            case PR_UNMAP:
            {
                g_pr.rc = kos_frame_unmap(CH_PR_FRAME, CH_PR_SPACE, g_pr_va2);
                pr_edited();
                break;
            }
            case PR_CLOSE:
            {
                g_pr.rc = kos_handle_close(CH_PR_FRAME);
                pr_edited();
                break;
            }
            case PR_LATE_PARAMS:
            {
                g_pr.rc = kos_frame_map(CH_PR_FRAME, CH_PR_SPACE, g_pr_va, 0);
                if (g_pr.rc == 0)
                {
                    memcpy(reinterpret_cast<void*>(g_pr_va), g_pl_src, g_pl_len);
                }
                break;
            }
            case PR_FLIP:
            {
                g_flip.task = KOS_TASK_NONE;
                g_flip.mem_base = g_pr.blk;
                g_flip.mem_size = g_pr.bytes;
                g_pr.rc = 0;
                break;
            }
            default:
            {
                g_pr.rc = 0;
                break;
            }
        }
    }
    void pr_editor(void*) // caps: done@1, note@2, frame@3, space@4
    {
        uint32_t bits = 0;
        g_pr.woke = -KOS_EINVAL;
        if (kos_notify_bind(CH_PR_NOTE) == 0)
        {
            // Outranking main, this parks before main runs again.
            kos_sem_post(CH_DONE);
            g_pr.woke = kos_notify_wait(CH_PR_NOTE, 0xFFFFFFFFu, STALL_TOLERANT_US, &bits);
        }
        g_pr.in_call = g_pr_in_call;
        g_pr.main_live = kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_LIVE, g_pr_main);
        pr_edit();
        kos_sem_post(CH_DONE);
    }
    struct PrRun
    {
        int32_t rc;
        uint64_t refusals;
        PrEdit seen;
    };
    // Spawns the editor for `action`, arms the line at main's next window, and runs `call`.
    template <typename Call>
    PrRun pr_run(kos_cap_t note, kos_cap_t fcap, kos_cap_t acap, int action, void* blk,
                 uint32_t bytes, Call call)
    {
        PrRun out = {-99, 0, {}};
        g_pr = {action, blk, bytes, 99, 99, 99, 99, 99};
        kos_cap_grant const caps[] = {{g_done, CH_FULL}, {note, CH_FULL},
                                      {fcap, KOS_CAP_TRANSFER}, {acap, KOS_CAP_TRANSFER}};
        uint8_t ncaps = 4;
        if (fcap == KOS_CAP_NONE)
        {
            ncaps = 2;
        }
        // Pinned with main to the core the line is claimed on, so the wake preempts main there.
        auto const editor = irq_spawn(pr_editor, nullptr, "pred", PR_PRIO, caps, ncaps,
                                      KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                      KOS_AUTH_MEMORY | KOS_AUTH_TASKS);
        if (not editor.valid())
        {
            return out;
        }
        wait_n(1);
        uint64_t const r0 = pr_refusals();
        (void)kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_ARM, static_cast<uint64_t>(PR_LINE));
        g_pr_in_call = 1;
        out.rc = call();
        g_pr_in_call = 0;
        out.refusals = pr_refusals() - r0;
        wait_n(1);
        (void)editor.join(UA_US);
        out.seen = g_pr;
        return out;
    }
    // A line claimed and bound to a notification main's editors and slayers wait on.
    struct PrLine
    {
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        bool ok = false;
        PrLine()
        {
            ok = kos_irq_claim(PR_LINE, KOS_IRQ_EDGE, &irq) == 0
                 and kos_notify_create(&note) == 0 and kos_irq_bind_notify(irq, note) == 0
                 and kos_irq_ack(irq) == 0;
        }
        ~PrLine()
        {
            (void)kos_handle_close(irq);
            (void)kos_handle_close(note);
        }
    };
    bool pr_syncs()
    {
        return kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 1) != 0 and UA_PER != 0;
    }
    void t_presync_race()
    {
        settle_exits();
        if (not pr_syncs())
        {
            tap::skip("no cacheable kernel view of a non-cacheable frame here");
            return;
        }
        uint64_t const g = static_cast<uint64_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        uint32_t const bytes = static_cast<uint32_t>(PR_PAGES * g);
        uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, PR_PAGES);
        g_pr_va = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 2u * PR_PAGES);
        g_pr_va2 = g_pr_va + PR_PAGES * g;
        g_pr_main = kos_thread_self();
        // One block for every round: the self-grant round leaves it non-cacheable, and a
        // non-cacheable handoff or window syncs whatever non-cacheable mappings it has.
        void* const blk = kos_ram_alloc(bytes);
        PrLine line;
        kos_task_t into = KOS_TASK_NONE;
        if (seed == 0 or g_pr_va == 0 or not line.ok or blk == nullptr
            or kos_task_create(nullptr, 0, 0, &into) != 0)
        {
            tap::skip("no frame run, reservation, task, line or notification left for the race");
            return;
        }
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFull);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        auto frame_map = [&] {
            int32_t const rc = kos_frame_map(fcap, acap, g_pr_va, KOS_MEM_NOCACHE);
            (void)kos_frame_unmap(fcap, acap, g_pr_va);
            return rc;
        };
        kos_task_t t = KOS_TASK_NONE;
        auto handoff = [&] {
            int32_t const rc = kos_task_create(blk, bytes, KOS_MEM_NOCACHE, &t);
            (void)kos_task_kill(t);
            return rc;
        };
        PrRun run[PR_ACTIONS] = {};
        run[PR_CAP_MAP] = pr_run(line.note, fcap, acap, PR_CAP_MAP, nullptr, 0, frame_map);
        run[PR_SELF_GRANT] = pr_run(line.note, fcap, acap, PR_SELF_GRANT, blk, bytes, handoff);
        run[PR_WINDOW] = pr_run(line.note, fcap, acap, PR_WINDOW, blk, bytes, handoff);
        run[PR_HANDOFF] = pr_run(line.note, fcap, acap, PR_HANDOFF, blk, bytes, [&] {
            kos_window const win = {reinterpret_cast<uintptr_t>(blk), bytes, KOS_WINDOW_MEMORY,
                                    KOS_WINDOW_UNCACHED};
            auto const c = kos::thread::create(ua_noop, nullptr, "prh", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0, &win, 1, nullptr,
                                               0, 0, nullptr, into);
            int32_t const rc = c.error();
            if (c.valid())
            {
                (void)c.join(UA_US);
            }
            return rc;
        });
        run[PR_FAULTED_OUT] = pr_run(line.note, fcap, acap, PR_FAULTED_OUT, blk, bytes, handoff);
        run[PR_FAILED_SPAWN] = pr_run(line.note, fcap, acap, PR_FAILED_SPAWN, blk, bytes, handoff);
        // A mapping of the run's own type that stood before the call: removing it changes
        // nothing the call synced.
        int32_t const stood = kos_frame_map(fcap, acap, g_pr_va2, KOS_MEM_NOCACHE);
        run[PR_UNMAP] = pr_run(line.note, fcap, acap, PR_UNMAP, nullptr, 0, frame_map);
        run[PR_CLOSE] = pr_run(line.note, fcap, acap, PR_CLOSE, nullptr, 0, frame_map);
        (void)kos_task_kill(into);
        settle_exits();
        (void)kos_handle_close(fcap);
        (void)kos_handle_close(acap);
        char const* const names[PR_FLIP] = {"cap map", "self-grant",   "window", "handoff",
                                            "lost handle", "failed spawn", "unmap", "close"};
        for (int i = 0; i < PR_FLIP; i++)
        {
            tap::diag("%s in the window: woke %ld, main's record live %lu then %lu, edit %ld;"
                      " main's call %ld after %lu rounds sent back",
                      names[i], static_cast<long>(run[i].seen.woke),
                      static_cast<unsigned long>(run[i].seen.main_live),
                      static_cast<unsigned long>(run[i].seen.main_live_after),
                      static_cast<long>(run[i].seen.rc), static_cast<long>(run[i].rc),
                      static_cast<unsigned long>(run[i].refusals));
        }
        for (int i = PR_CAP_MAP; i <= PR_HANDOFF; i++)
        {
            TAP_CHECK(run[i].seen.woke == 0 and run[i].seen.main_live == 1
                      and run[i].seen.main_live_after == 0 and run[i].seen.rc == 0
                      and run[i].rc == 0 and run[i].refusals == 1);
        }
        TAP_CHECK(run[PR_FAULTED_OUT].seen.woke == 0
                  and run[PR_FAULTED_OUT].seen.rc == -KOS_EFAULT
                  and run[PR_FAULTED_OUT].seen.main_live_after == 0
                  and run[PR_FAULTED_OUT].rc == 0 and run[PR_FAULTED_OUT].refusals == 1);
        TAP_CHECK(run[PR_FAILED_SPAWN].seen.woke == 0
                  and run[PR_FAILED_SPAWN].seen.rc == -KOS_EINVAL
                  and run[PR_FAILED_SPAWN].seen.main_live_after == 1
                  and run[PR_FAILED_SPAWN].rc == 0 and run[PR_FAILED_SPAWN].refusals == 0);
        TAP_CHECK(stood == 0);
        for (int i = PR_UNMAP; i <= PR_CLOSE; i++)
        {
            TAP_CHECK(run[i].seen.woke == 0 and run[i].seen.rc == 0
                      and run[i].seen.main_live_after == 1 and run[i].rc == 0
                      and run[i].refusals == 0);
        }
    }

    // A sibling rewriting a spawn's parameters while the spawn works outside the lock changes
    // nothing it does: the kernel copied them at the call's entry.
    void t_presync_flip()
    {
        settle_exits();
        if (not pr_syncs())
        {
            tap::skip("no sync owed here, so a rewrite in the window cannot send a spawn round");
            return;
        }
        uint64_t const g = static_cast<uint64_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        uint32_t const bytes = static_cast<uint32_t>(PR_PAGES * g);
        void* const blk = kos_ram_alloc(bytes);
        void* const grant = kos_ram_alloc(static_cast<uint32_t>(g));
        PrLine line;
        kos_task_t t = KOS_TASK_NONE;
        if (blk == nullptr or grant == nullptr or not line.ok
            or kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            tap::skip("no reservation, task, line or notification left");
            return;
        }
        g_pr_main = kos_thread_self();
        kos_window const win = {reinterpret_cast<uintptr_t>(blk), bytes, KOS_WINDOW_MEMORY,
                                KOS_WINDOW_UNCACHED};
        g_flip = {};
        g_flip.entry = ua_noop;
        g_flip.name = "prf";
        g_flip.prio = 10;
        g_flip.windows = &win;
        g_flip.window_count = 1;
        g_flip.task = t;
        kos_thread_t child = KOS_THREAD_NONE;
        PrRun const run = pr_run(line.note, KOS_CAP_NONE, KOS_CAP_NONE, PR_FLIP, grant,
                                 static_cast<uint32_t>(g),
                                 [&] { return kos_thread_create(&g_flip, &child); });
        int32_t const joined = kos_thread_join(child, UA_US);
        (void)kos_task_kill(t);
        settle_exits();
        tap::diag("parameters rewritten in the window: woke %ld, spawn %ld after %lu rounds sent"
                  " back, joined %ld",
                  static_cast<long>(run.seen.woke), static_cast<long>(run.rc),
                  static_cast<unsigned long>(run.refusals), static_cast<long>(joined));
        TAP_CHECK(run.seen.woke == 0 and run.seen.in_call == 1);
        TAP_CHECK(run.rc == 0 and run.refusals == 0 and joined == 0);
    }

    // A thread slain inside its call's work outside the lock leaves no live record and no
    // staged run: a sync of a frame capability's map, the clear of a new reservation, and the
    // copy of a new space's static data.
    enum PsCall
    {
        PS_MAP,
        PS_CLEAR,
        PS_COPY,
        PS_CALLS
    };
    constexpr int CH_PS_FRAME = 1;
    constexpr int CH_PS_SPACE = 2;
    int g_ps_call = PS_MAP;
    uint32_t g_ps_bytes = 0;
    int32_t g_ps_woke = 99;
    int32_t g_ps_slay = 99;
    uint64_t g_ps_live = 99;
    uint64_t g_ps_all = 99;
    void ps_victim(void*) // caps: frame@1, space@2
    {
        (void)kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_ARM, static_cast<uint64_t>(PR_LINE));
        if (g_ps_call == PS_MAP)
        {
            (void)kos_frame_map(CH_PS_FRAME, CH_PS_SPACE, g_pr_va, KOS_MEM_NOCACHE);
        }
        else if (g_ps_call == PS_CLEAR)
        {
            (void)kos_ram_alloc(g_ps_bytes);
        }
        else
        {
            kos_task_t t = KOS_TASK_NONE;
            (void)kos_task_create(nullptr, 0, 0, &t);
        }
    }
    void ps_slayer(void*) // caps: done@1, note@2, frame@3, space@4
    {
        uint32_t bits = 0;
        g_ps_woke = -KOS_EINVAL;
        if (kos_notify_bind(CH_PR_NOTE) != 0)
        {
            kos_sem_post(CH_DONE);
            kos_sem_post(CH_DONE);
            return;
        }
        kos_cap_grant const caps[] = {{CH_PR_FRAME, KOS_CAP_TRANSFER},
                                      {CH_PR_SPACE, KOS_CAP_TRANSFER}};
        uint8_t ncaps = 0;
        if (g_ps_call == PS_MAP)
        {
            ncaps = 2;
        }
        // Below this thread, above main, on this thread's core.
        auto const victim = kos::thread::create_caps(
            ps_victim, nullptr, "psv", PR_PRIO - 1, caps, ncaps, KOS_POLICY_FIFO, 0, false,
            nullptr, 0, KOS_AUTH_MEMORY | KOS_AUTH_TASKS, nullptr, KOS_TASK_NONE, nullptr, 0,
            TAP_PIN_CORE);
        kos_sem_post(CH_DONE);
        if (victim.valid())
        {
            g_ps_woke = kos_notify_wait(CH_PR_NOTE, 0xFFFFFFFFu, UA_US, &bits);
            g_ps_slay = kos_thread_slay(victim.id(), UA_US);
            g_ps_live = kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_LIVE, victim.id());
            g_ps_all = kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_LIVE, 0);
        }
        kos_sem_post(CH_DONE);
    }
    struct PsSeen
    {
        int32_t woke;
        int32_t slay;
        uint64_t live;
        uint64_t all;
        uint64_t frames_before;
        uint64_t frames_after;
    };
    // `fcap` and `acap` only for PS_MAP.
    PsSeen ps_run(kos_cap_t note, int call, kos_cap_t fcap, kos_cap_t acap)
    {
        PsSeen out = {99, 99, 99, 99, 0, 0};
        g_ps_call = call;
        g_ps_woke = 99;
        g_ps_slay = 99;
        g_ps_live = 99;
        g_ps_all = 99;
        settle_exits();
        out.frames_before = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        kos_cap_grant const caps[] = {{g_done, CH_FULL}, {note, CH_FULL},
                                      {fcap, KOS_CAP_TRANSFER}, {acap, KOS_CAP_TRANSFER}};
        uint8_t ncaps = 2;
        if (call == PS_MAP)
        {
            ncaps = 4;
        }
        auto const slayer = irq_spawn(ps_slayer, nullptr, "pss", PR_PRIO, caps, ncaps,
                                      KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                      KOS_AUTH_MEMORY | KOS_AUTH_TASKS);
        if (slayer.valid())
        {
            wait_n(2);
            (void)slayer.join(UA_US);
        }
        settle_exits();
        out.frames_after = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        out.woke = g_ps_woke;
        out.slay = g_ps_slay;
        out.live = g_ps_live;
        out.all = g_ps_all;
        return out;
    }
    void t_presync_slain()
    {
        settle_exits();
        uint64_t const g = static_cast<uint64_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        g_ps_bytes = static_cast<uint32_t>(PR_PAGES * g);
        uint64_t seed = 0;
        if (pr_syncs())
        {
            seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, PR_PAGES);
            g_pr_va = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, PR_PAGES);
        }
        PrLine line;
        if (not line.ok)
        {
            tap::skip("no line or notification left");
            return;
        }
        // The copy is staged only once the snapshot exists.
        kos_task_t warm = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &warm) == 0)
        {
            (void)kos_task_kill(warm);
        }
        uint64_t const before = kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_LIVE, 0);
        PsSeen seen[PS_CALLS] = {};
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFull);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        if (seed != 0)
        {
            seen[PS_MAP] = ps_run(line.note, PS_MAP, fcap, acap);
            (void)kos_handle_close(fcap);
            (void)kos_handle_close(acap);
        }
        seen[PS_CLEAR] = ps_run(line.note, PS_CLEAR, KOS_CAP_NONE, KOS_CAP_NONE);
        seen[PS_COPY] = ps_run(line.note, PS_COPY, KOS_CAP_NONE, KOS_CAP_NONE);
        char const* const names[PS_CALLS] = {"sync", "clear", "copy"};
        for (int i = 0; i < PS_CALLS; i++)
        {
            tap::diag("slain mid-%s: woke %ld, slay %ld, its record live %lu, live records %lu"
                      " (%lu before), frames free %lu then %lu",
                      names[i], static_cast<long>(seen[i].woke), static_cast<long>(seen[i].slay),
                      static_cast<unsigned long>(seen[i].live),
                      static_cast<unsigned long>(seen[i].all),
                      static_cast<unsigned long>(before),
                      static_cast<unsigned long>(seen[i].frames_before),
                      static_cast<unsigned long>(seen[i].frames_after));
        }
        for (int i = 0; i < PS_CALLS; i++)
        {
            if (i == PS_MAP and seed == 0)
            {
                continue;
            }
            TAP_CHECK(seen[i].woke == 0 and seen[i].slay == 0);
            TAP_CHECK(seen[i].live == 0 and seen[i].all == before);
            TAP_CHECK(seen[i].frames_after == seen[i].frames_before);
        }
    }

    // What a stage costs and gives back: nothing for a call refused its authority, its
    // out-word or its arguments, a pool refusal failing the call rather than sending it round,
    // and a stage the call never took freed when it ends.
    enum PuMalformed
    {
        PU_WINDOWS,
        PU_GRANT,
        PU_AUTHORITY,
        PU_STACK,
        PU_HANDOFF,
        PU_BOTH,
        PU_MALFORMED
    };
    uint64_t g_pu_staged = 99;
    int32_t g_pu_task = 99;
    int32_t g_pu_spawn = 99;
    void* g_pu_grant = nullptr;
    // A data grant no spawn takes, shared: range slots cannot be freed through the user API.
    void* pu_grant()
    {
        if (g_pu_grant == nullptr)
        {
            g_pu_grant = kos_ram_alloc(64);
        }
        return g_pu_grant;
    }
    void pu_worker(void*) // caps: done@1
    {
        uint64_t const s0 = pr_staged();
        kos_task_t t = KOS_TASK_NONE;
        g_pu_task = kos_task_create(nullptr, 0, 0, &t);
        g_pu_spawn = kos::thread::create(ua_noop, nullptr, "pun", 10, KOS_POLICY_FIFO, 0, false,
                                         g_pu_grant, 64)
                         .error();
        g_pu_staged = pr_staged() - s0;
        kos_sem_post(CH_DONE);
    }
    void t_presync_staged()
    {
        settle_exits();
        // The copy is staged only once the snapshot exists.
        kos_task_t warm = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &warm) == 0)
        {
            (void)kos_task_kill(warm);
        }
        (void)pu_grant();
        kos_cap_grant const caps[] = {{g_done, CH_FULL}};
        auto const worker = kos::thread::create_caps(pu_worker, nullptr, "puw", 10, caps, 1,
                                                     KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                     KOS_AUTH_MEMORY);
        TAP_CHECK(worker.valid());
        if (worker.valid())
        {
            wait_n(1);
            (void)worker.join(UA_US);
        }
        settle_exits();
        // Nor does a call whose out-word the kernel refuses.
        uint64_t const b0 = pr_staged();
        int32_t const no_out_task = kos_task_create(nullptr, 0, 0, nullptr);
        kos_thread_params np = {};
        np.entry = ua_noop;
        np.name = "puo";
        np.prio = 10;
        np.mem_base = g_pu_grant;
        np.mem_size = 64;
        int32_t const no_out_spawn = kos_thread_create(&np, nullptr);
        uint64_t const b1 = pr_staged();
        settle_exits();
        uint64_t const f0 = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        (void)kos_aspace_probe(KOS_ASPACE_OP_POOL_FAIL_IN, 1);
        uint64_t const r0 = pr_refusals();
        kos_task_t shorted = KOS_TASK_NONE;
        int32_t const short_rc = kos_task_create(nullptr, 0, 0, &shorted);
        uint64_t const r1 = pr_refusals();
        (void)kos_aspace_probe(KOS_ASPACE_OP_POOL_FAIL_IN, 0);
        settle_exits();
        uint64_t const f1 = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        // Nor does a spawn that would build a task and that its arguments alone refuse: a
        // window list naming no memory this task can read, a data grant it does not own, an
        // authority past the defined ones, a misaligned stack, a grant that is part of a
        // reservation only. One with a bad window list too answers for its windows.
        kos_thread_params bad[PU_MALFORMED];
        for (kos_thread_params& b : bad)
        {
            b = {};
            b.entry = ua_noop;
            b.name = "pur";
            b.prio = 10;
            b.mem_base = g_pu_grant;
            b.mem_size = 64;
        }
        bad[PU_WINDOWS].windows =
            reinterpret_cast<kos_window const*>(static_cast<uintptr_t>(0x10u));
        bad[PU_WINDOWS].window_count = 1;
        bad[PU_GRANT].mem_base = reinterpret_cast<void*>(static_cast<uintptr_t>(0x10u));
        bad[PU_AUTHORITY].authority = 0x80000000u;
        bad[PU_STACK].stack_base = static_cast<unsigned char*>(g_pu_grant) + 8;
        bad[PU_STACK].stack_size = 64u * 1024u;
        bad[PU_HANDOFF].mem_base = static_cast<unsigned char*>(g_pu_grant) + 16;
        bad[PU_HANDOFF].mem_size = 32;
        bad[PU_BOTH] = bad[PU_HANDOFF];
        bad[PU_BOTH].windows = bad[PU_WINDOWS].windows;
        bad[PU_BOTH].window_count = 1;
        int32_t bad_rc[PU_MALFORMED];
        uint64_t bad_staged[PU_MALFORMED];
        for (int i = 0; i < PU_MALFORMED; i++)
        {
            uint64_t const s0 = pr_staged();
            kos_thread_t child = KOS_THREAD_NONE;
            bad_rc[i] = kos_thread_create(&bad[i], &child);
            bad_staged[i] = pr_staged() - s0;
        }
        // A stage the call never took, refused past its arguments for a grant naming no
        // handle, is freed when it ends.
        kos_cap_grant const no_cap[] = {{0x7FFEu, KOS_CAP_SIGNAL}};
        kos_thread_params late = bad[PU_STACK];
        late.stack_base = nullptr;
        late.stack_size = 0;
        late.caps = no_cap;
        late.cap_count = 1;
        uint64_t const l0 = pr_staged();
        kos_thread_t late_child = KOS_THREAD_NONE;
        int32_t const late_rc = kos_thread_create(&late, &late_child);
        uint64_t const l1 = pr_staged();
        settle_exits();
        uint64_t const f2 = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        tap::diag("unauthorised: task %ld, spawn %d, %lu granules staged; pool refusing the"
                  " stage: %ld after %lu rounds, frames %lu then %lu, then %lu",
                  static_cast<long>(g_pu_task), static_cast<int>(g_pu_spawn),
                  static_cast<unsigned long>(g_pu_staged), static_cast<long>(short_rc),
                  static_cast<unsigned long>(r1 - r0), static_cast<unsigned long>(f0),
                  static_cast<unsigned long>(f1), static_cast<unsigned long>(f2));
        char const* const bad_names[PU_MALFORMED] = {"windows", "grant", "authority", "stack",
                                                     "part of a reservation", "both"};
        for (int i = 0; i < PU_MALFORMED; i++)
        {
            tap::diag("malformed %s: spawn %ld staging %lu granules", bad_names[i],
                      static_cast<long>(bad_rc[i]), static_cast<unsigned long>(bad_staged[i]));
        }
        tap::diag("no out-word: task %ld, spawn %ld, %lu granules staged",
                  static_cast<long>(no_out_task), static_cast<long>(no_out_spawn),
                  static_cast<unsigned long>(b1 - b0));
        TAP_CHECK(g_pu_task == -KOS_EPERM and g_pu_spawn == -KOS_EPERM and g_pu_staged == 0);
        TAP_CHECK(no_out_task == -KOS_EINVAL and no_out_spawn == -KOS_EINVAL and b1 == b0);
        TAP_CHECK(short_rc == -KOS_ENOMEM and shorted == KOS_TASK_NONE and r1 == r0);
        TAP_CHECK(f1 == f0);
        TAP_CHECK(bad_rc[PU_WINDOWS] == -KOS_EFAULT and bad_rc[PU_GRANT] == -KOS_EPERM
                  and bad_rc[PU_AUTHORITY] == -KOS_EINVAL and bad_rc[PU_STACK] == -KOS_EINVAL);
        TAP_CHECK(bad_rc[PU_HANDOFF] == -KOS_EPERM and bad_rc[PU_BOTH] == -KOS_EFAULT);
        for (uint64_t const staged : bad_staged)
        {
            TAP_CHECK(staged == 0);
        }
        tap::diag("refused past its arguments: spawn %ld staging %lu granules",
                  static_cast<long>(late_rc), static_cast<unsigned long>(l1 - l0));
        TAP_CHECK(late_rc == -KOS_EBADF and l1 > l0 and f2 == f0);
    }

    // kos_thread_kill ends a call that keeps going round, at its next round, and the next
    // thread in the killed one's slot inherits nothing of its record.
    constexpr int CH_PC_FRAME = 1;
    constexpr int CH_PC_SPACE = 2;
    constexpr uint64_t PC_STEP_NS = 1000000u;
    enum PcMode
    {
        PC_CANCEL,
        PC_REUSE,
        PC_MODES
    };
    int g_pc_mode = PC_CANCEL;
    int32_t g_pc_kill = 99;
    int32_t g_pc_join = 99;
    int32_t g_pc_rc = 99;
    uint64_t g_pc_rounds = 99;
    uint32_t g_pc_slot[PC_MODES] = {};
    volatile int g_pc_returned = 0;
    void pc_victim(void*) // caps: frame@1, space@2
    {
        uint64_t const r0 = pr_refusals();
        if (g_pc_mode == PC_CANCEL)
        {
            pr_drop_next(PR_DROPS_FOREVER);
        }
        g_pc_rc = kos_frame_map(CH_PC_FRAME, CH_PC_SPACE, g_pr_va, KOS_MEM_NOCACHE);
        g_pc_rounds = pr_refusals() - r0;
        if (g_pc_rc == 0)
        {
            (void)kos_frame_unmap(CH_PC_FRAME, CH_PC_SPACE, g_pr_va);
        }
        g_pc_returned = 1;
    }
    void pc_killer(void*) // caps: done@1, frame@2, space@3
    {
        uint64_t const r0 = pr_refusals();
        kos_cap_grant const caps[] = {{2, KOS_CAP_TRANSFER}, {3, KOS_CAP_TRANSFER}};
        auto const victim = kos::thread::create_caps(
            pc_victim, nullptr, "pcv", PR_PRIO - 1, caps, 2, KOS_POLICY_FIFO, 0, false, nullptr,
            0, KOS_AUTH_MEMORY, nullptr, KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        if (victim.valid())
        {
            g_pc_slot[g_pc_mode] = victim.id() & 0xFFFFu;
            if (g_pc_mode == PC_CANCEL)
            {
                // Killed only once its call has gone round, wherever the host held this core.
                uint64_t const give_up = kos_clock_now() + STALL_TOLERANT_US * 1000ull;
                while (pr_refusals() == r0 and kos_clock_now() < give_up)
                {
                    kos_sleep_ns(PC_STEP_NS);
                }
                g_pc_kill = kos_thread_kill(victim.id());
                g_pc_join = victim.join(STALL_TOLERANT_US);
            }
            else
            {
                g_pc_join = victim.join(STALL_TOLERANT_US);
                if (g_pc_join != 0)
                {
                    g_pc_kill = kos_thread_kill(victim.id());
                }
            }
            if (g_pc_join != 0 and victim.join(UA_US) != 0)
            {
                (void)kos_thread_slay(victim.id(), UA_US);
            }
        }
        kos_sem_post(CH_DONE);
    }
    void pc_round(int mode, kos_cap_t fcap, kos_cap_t acap)
    {
        g_pc_mode = mode;
        g_pc_kill = 99;
        g_pc_join = 99;
        g_pc_rc = 99;
        g_pc_rounds = 99;
        g_pc_returned = 0;
        kos_cap_grant const caps[] = {{g_done, CH_FULL},
                                      {fcap, KOS_CAP_TRANSFER},
                                      {acap, KOS_CAP_TRANSFER}};
        auto const killer = kos::thread::create_caps(
            pc_killer, nullptr, "pck", PR_PRIO, caps, 3, KOS_POLICY_FIFO, 0, false, nullptr, 0,
            KOS_AUTH_MEMORY, nullptr, KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(killer.valid());
        if (killer.valid())
        {
            wait_n(1);
            (void)killer.join(UA_US);
        }
        settle_exits();
    }
    void t_presync_cancel()
    {
        settle_exits();
        if (not pr_syncs())
        {
            tap::skip("no cacheable kernel view of a non-cacheable frame here");
            return;
        }
        uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, PR_PAGES);
        g_pr_va = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, PR_PAGES);
        if (seed == 0 or g_pr_va == 0)
        {
            tap::skip("no frame run left");
            return;
        }
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFull);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        uint64_t const r0 = pr_refusals();
        pc_round(PC_CANCEL, fcap, acap);
        int32_t const kill = g_pc_kill;
        int32_t const join = g_pc_join;
        int const returned = g_pc_returned;
        uint64_t const rounds = pr_refusals() - r0;
        pc_round(PC_REUSE, fcap, acap);
        (void)kos_handle_close(fcap);
        (void)kos_handle_close(acap);
        tap::diag("killed while going round: kill %ld, join %ld, returned %d, %lu rounds",
                  static_cast<long>(kill), static_cast<long>(join), returned,
                  static_cast<unsigned long>(rounds));
        tap::diag("its slot %u reused by slot %u: map %ld after %lu rounds, returned %d",
                  static_cast<unsigned>(g_pc_slot[PC_CANCEL]),
                  static_cast<unsigned>(g_pc_slot[PC_REUSE]), static_cast<long>(g_pc_rc),
                  static_cast<unsigned long>(g_pc_rounds), g_pc_returned);
        TAP_CHECK(kill == 0 and join == 0 and returned == 0);
        TAP_CHECK(rounds > 0);
        TAP_CHECK(g_pc_slot[PC_REUSE] == g_pc_slot[PC_CANCEL]);
        TAP_CHECK(g_pc_returned == 1 and g_pc_rc == 0 and g_pc_rounds == 0);
    }

    // A spawn whose parameters or window list were unreadable at its entry fails, and is not
    // sent round by a sibling mapping them before its locked pass.
    constexpr uint64_t PR_IDLE_WINDOW = 1ull << 25;
    int32_t g_pl_rc = 99;
    uint64_t g_pl_rounds = 99;
    int32_t g_pl_kill = 99;
    volatile int g_pl_returned = 0;
    void pl_victim(void* arg)
    {
        uint64_t const r0 = pr_refusals();
        (void)kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_ARM,
                               static_cast<uint64_t>(PR_LINE) | PR_IDLE_WINDOW);
        kos_thread_t child = KOS_THREAD_NONE;
        g_pl_rc = kos_thread_create(static_cast<kos_thread_params const*>(arg), &child);
        g_pl_rounds = pr_refusals() - r0;
        g_pl_returned = 1;
    }
    void pl_watcher(void* arg) // caps: done@1
    {
        auto const victim = kos::thread::create_caps(
            pl_victim, arg, "plv", PR_PRIO - 1, nullptr, 0, KOS_POLICY_FIFO, 0, false, nullptr,
            0, KOS_AUTH_MEMORY | KOS_AUTH_TASKS, nullptr, KOS_TASK_NONE, nullptr, 0,
            TAP_PIN_CORE);
        if (victim.valid() and victim.join(STALL_TOLERANT_US) != 0)
        {
            g_pl_kill = kos_thread_kill(victim.id());
            if (victim.join(UA_US) != 0)
            {
                (void)kos_thread_slay(victim.id(), UA_US);
            }
        }
        kos_sem_post(CH_DONE);
    }
    struct PlRun
    {
        int32_t woke;
        int32_t map;
        int32_t rc;
        uint64_t rounds;
        int returned;
        int32_t kill;
    };
    // The editor maps the frame capability at g_pr_va in the victim's window and copies `src`
    // there.
    PlRun pl_run(kos_cap_t note, kos_cap_t fcap, kos_cap_t acap, void const* src, size_t len,
                 kos_thread_params const* params)
    {
        PlRun out = {99, 99, 99, 99, 0, 99};
        g_pl_src = src;
        g_pl_len = len;
        g_pl_rc = 99;
        g_pl_rounds = 99;
        g_pl_kill = 99;
        g_pl_returned = 0;
        g_pr_main = kos_thread_self();
        g_pr = {PR_LATE_PARAMS, nullptr, 0, 99, 99, 99, 99, 99};
        kos_cap_grant const ecaps[] = {{g_done, CH_FULL},
                                       {note, CH_FULL},
                                       {fcap, KOS_CAP_TRANSFER},
                                       {acap, KOS_CAP_TRANSFER}};
        auto const editor =
            irq_spawn(pr_editor, nullptr, "pled", PR_PRIO, ecaps, 4, KOS_POLICY_FIFO, 0, false,
                      nullptr, 0, KOS_AUTH_MEMORY | KOS_AUTH_TASKS);
        if (not editor.valid())
        {
            return out;
        }
        wait_n(1);
        kos_cap_grant const wcaps[] = {{g_done, CH_FULL}};
        auto const watcher =
            irq_spawn(pl_watcher, const_cast<kos_thread_params*>(params), "plw", PR_PRIO, wcaps,
                      1, KOS_POLICY_FIFO, 0, false, nullptr, 0, KOS_AUTH_MEMORY | KOS_AUTH_TASKS);
        if (watcher.valid())
        {
            wait_n(2);
            (void)watcher.join(UA_US);
        }
        (void)editor.join(UA_US);
        settle_exits();
        (void)kos_frame_unmap(fcap, acap, g_pr_va);
        out = {g_pr.woke, g_pr.rc, g_pl_rc, g_pl_rounds, g_pl_returned, g_pl_kill};
        return out;
    }
    void t_presync_late_params()
    {
        settle_exits();
        kos_task_t warm = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &warm) == 0)
        {
            (void)kos_task_kill(warm);
        }
        uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 1);
        g_pr_va = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 1);
        void* const grant = pu_grant();
        void* const target = snp_block();
        PrLine line;
        if (seed == 0 or g_pr_va == 0 or grant == nullptr or target == nullptr or not line.ok)
        {
            tap::skip("no frame run, reservation, line or notification left");
            return;
        }
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFull);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        static kos_thread_params params = {};
        params = {};
        params.entry = ua_noop;
        params.name = "pll";
        params.prio = 10;
        params.mem_base = grant;
        params.mem_size = 64;
        PlRun run[2] = {};
        run[0] = pl_run(line.note, fcap, acap, &params, sizeof(params),
                        reinterpret_cast<kos_thread_params const*>(g_pr_va));
        kos_window const win = {reinterpret_cast<uintptr_t>(target), SNP_BLK, KOS_WINDOW_MEMORY,
                                0};
        static kos_thread_params windowed = {};
        windowed = params;
        windowed.windows = reinterpret_cast<kos_window const*>(g_pr_va);
        windowed.window_count = 1;
        run[1] = pl_run(line.note, fcap, acap, &win, sizeof(win), &windowed);
        (void)kos_handle_close(fcap);
        (void)kos_handle_close(acap);
        char const* const names[2] = {"parameters", "window list"};
        for (int i = 0; i < 2; i++)
        {
            tap::diag("%s mapped in the window: woke %ld, map %ld; spawn %ld after %lu rounds"
                      " sent back, returned %d, kill %ld",
                      names[i], static_cast<long>(run[i].woke), static_cast<long>(run[i].map),
                      static_cast<long>(run[i].rc), static_cast<unsigned long>(run[i].rounds),
                      run[i].returned, static_cast<long>(run[i].kill));
            TAP_CHECK(run[i].woke == 0 and run[i].map == 0);
            TAP_CHECK(run[i].returned == 1 and run[i].rc == -KOS_EFAULT and run[i].rounds == 0);
        }
    }

    // A reservation's clear and a new space's copy of the image's static data run outside the
    // lock with an interrupt window per granule: an interrupt pending at the start is taken
    // while the call is still in its system call.
    void t_out_of_lock_windows()
    {
        settle_exits();
        uint64_t const g = static_cast<uint64_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        uint32_t const bytes = static_cast<uint32_t>(PR_PAGES * g);
        PrLine line;
        if (not line.ok)
        {
            tap::skip("no line or notification left");
            return;
        }
        // The snapshot is taken once, under the lock, by the first explicit task.
        kos_task_t warm = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &warm) == 0)
        {
            (void)kos_task_kill(warm);
        }
        g_pr_main = kos_thread_self();
        void* got = nullptr;
        uint64_t const l0 = kos_aspace_probe(KOS_ASPACE_OP_LOCKED_PAGES, 0);
        uint64_t const w0 = pr_own_windows();
        uint64_t const s0 = pr_granules();
        uint64_t const g0 = pr_staged();
        PrRun const clear = pr_run(line.note, KOS_CAP_NONE, KOS_CAP_NONE, PR_WATCH, nullptr, 0,
                                   [&] {
                                       got = kos_ram_alloc(bytes);
                                       return 0;
                                   });
        uint64_t const g1 = pr_staged();
        kos_task_t t = KOS_TASK_NONE;
        PrRun const copy = pr_run(line.note, KOS_CAP_NONE, KOS_CAP_NONE, PR_WATCH, nullptr, 0,
                                  [&] { return kos_task_create(nullptr, 0, 0, &t); });
        uint64_t const g2 = pr_staged();
        (void)kos_task_kill(t);
        PrRun const spawn = pr_run(line.note, KOS_CAP_NONE, KOS_CAP_NONE, PR_WATCH, nullptr, 0,
                                   [&] {
                                       if (got == nullptr)
                                       {
                                           return static_cast<int>(-KOS_ENOMEM);
                                       }
                                       auto const c = kos::thread::create(
                                           ua_noop, nullptr, "pri", 10, KOS_POLICY_FIFO, 0,
                                           false, got, bytes);
                                       int const rc = c.error();
                                       if (c.valid())
                                       {
                                           (void)c.join(UA_US);
                                       }
                                       return rc;
                                   });
        uint64_t const g3 = pr_staged();
        uint64_t const windows = pr_own_windows() - w0;
        uint64_t const synced = pr_granules() - s0;
        uint64_t const locked = kos_aspace_probe(KOS_ASPACE_OP_LOCKED_PAGES, 0) - l0;
        uint64_t const masked = kos_aspace_probe(KOS_ASPACE_OP_PRESYNC_MASKED, 0);
        settle_exits();
        tap::diag("clear of %lu granules: woke %ld in the call %d; image copy at task create:"
                  " woke %ld in the call %d, %lu granules; at a spawn: woke %ld in the call %d,"
                  " %lu granules",
                  static_cast<unsigned long>(g1 - g0), static_cast<long>(clear.seen.woke),
                  clear.seen.in_call, static_cast<long>(copy.seen.woke), copy.seen.in_call,
                  static_cast<unsigned long>(g2 - g1), static_cast<long>(spawn.seen.woke),
                  spawn.seen.in_call, static_cast<unsigned long>(g3 - g2));
        tap::diag("main's own windows %lu over %lu granules, %lu under the lock; at most %lu ns"
                  " between two windows",
                  static_cast<unsigned long>(windows),
                  static_cast<unsigned long>((g3 - g0) + synced),
                  static_cast<unsigned long>(locked), static_cast<unsigned long>(masked >> 32));
        TAP_CHECK(got != nullptr and g1 - g0 == PR_PAGES);
        TAP_CHECK(clear.seen.woke == 0 and clear.seen.in_call == 1);
        TAP_CHECK(copy.rc == 0 and copy.seen.woke == 0 and copy.seen.in_call == 1 and g2 > g1);
        TAP_CHECK(spawn.rc == 0 and spawn.seen.woke == 0 and spawn.seen.in_call == 1
                  and g3 - g2 == g2 - g1);
        TAP_CHECK(windows == (g3 - g0) + synced);
        TAP_CHECK(locked == 0);
    }

    // New processes copy the saved startup globals, never a live process's mutable data.
    volatile uint64_t g_dt_word = 0xC0FFEEull;
    constexpr uint64_t DT_A = 0xC0FFEEull;
    constexpr uint64_t DT_B = 0xBADBADull;
    enum
    {
        DT_VALUE = 0,
        DT_FRAME = 1,
        DT_WORDS = 2
    };
    void dt_reader(void* arg) // caps: done@1
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        out[DT_VALUE] = g_dt_word;
        out[DT_FRAME] =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(&g_dt_word));
        kos_sem_post(CH_DONE);
    }
    // A receiver parks, a sibling unmaps its buffer and sends: both must get EFAULT. The sender
    // buffer is separate to exclude overlap errors. Both are pinned and the receiver outranks
    // the sibling, so validation precedes the unmap. Sender EFAULT proves delivery reached a
    // parked receiver; a boundary rejection would leave the sender to time out.
    constexpr size_t RU_LEN = 32;
    constexpr uint32_t RU_SEND_US = 200000;
    // The deadline turns broken ordering into a failure instead of an indefinite wait.
    constexpr uint32_t RU_RECV_US = 500000;
    constexpr int CH_RU_EP = 2;
    constexpr int CH_RU_FRAME = 3;
    constexpr int CH_RU_SPACE = 4;
    char g_ru_src[RU_LEN];
    Atomic<int32_t, Order::RELAXED> g_ru_unmap{1};
    Atomic<int32_t, Order::RELAXED> g_ru_sent{1};
    Atomic<int32_t, Order::RELAXED> g_ru_got{99};
    void ru_receiver(void* arg) // caps: done@1, E(WAIT)@2
    {
        struct kos_reply_recv_opts opts = {};
        opts.timeout_us = RU_RECV_US;
        opts.info.reply_cap = KOS_CAP_NONE;
        opts.ep = CH_RU_EP;
        g_ru_got = kos_reply_recv(KOS_CAP_NONE, arg, kos_call_lens_pack(0, RU_LEN), &opts);
        kos_sem_post(CH_DONE);
    }
    void ru_puller(void* arg) // caps: done@1, E(SIGNAL)@2, frame@3, space@4
    {
        g_ru_unmap = kos_frame_unmap(CH_RU_FRAME, CH_RU_SPACE,
                                     reinterpret_cast<uintptr_t>(arg));
        g_ru_sent = kos_send_timed(CH_RU_EP, g_ru_src, RU_LEN, RU_SEND_US);
        kos_sem_post(CH_DONE);
    }
    void t_recv_buf_unmapped()
    {
        uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0);
        if (seed == 0)
        {
            tap::skip("no frame capability to seed"); // 0 and not KOS_CAP_NONE: see the op
            return;
        }
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFull);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        uintptr_t const va = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 0);
        kos_cap_t ep = KOS_CAP_NONE;
        if (va == 0 or kos_endpoint_create(&ep) != 0)
        {
            (void)kos_handle_close(fcap);
            (void)kos_handle_close(acap);
            tap::skip("no seed window or no endpoint slot");
            return;
        }
        TAP_CHECK(kos_frame_map(fcap, acap, va, 0) == 0);
        for (size_t i = 0; i < RU_LEN; i++)
        {
            g_ru_src[i] = static_cast<char>('A' + (i & 15u));
        }
        g_ru_unmap = 1;
        g_ru_sent = 1;
        g_ru_got = 99;
        // No task named, so both are threads of main's task and the page the puller takes is
        // the receiver's own.
        kos_cap_grant rcaps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_WAIT}};
        kos_cap_grant pcaps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_SIGNAL},
                                 {fcap, KOS_CAP_TRANSFER}, {acap, KOS_CAP_TRANSFER}};
        int spawned = 0;
        if (kos::thread::create_caps(ru_receiver, reinterpret_cast<void*>(va), "rurcv",
                                     TAP_PRIO_PARKS, rcaps, 2, KOS_POLICY_FIFO, 0, false,
                                     nullptr, 0, 0, nullptr, KOS_TASK_NONE, nullptr, 0,
                                     TAP_PIN_CORE)
                .valid())
        {
            spawned++;
            if (kos::thread::create_caps(ru_puller, reinterpret_cast<void*>(va), "rupul",
                                         TAP_PRIO_AFTER, pcaps, 4, KOS_POLICY_FIFO, 0, false,
                                         nullptr, 0, KOS_AUTH_MEMORY, nullptr, KOS_TASK_NONE,
                                         nullptr, 0, TAP_PIN_CORE)
                    .valid())
            {
                spawned++;
            }
        }
        wait_n(spawned);
        (void)kos_frame_unmap(fcap, acap, va);
        (void)kos_handle_close(ep);
        (void)kos_handle_close(fcap);
        (void)kos_handle_close(acap);
        if (spawned < 2)
        {
            tap::skip("thread pool too small for the pair");
            return;
        }
        tap::diag("recv buffer unmapped: recv %ld, unmap %ld, send %ld",
                  static_cast<long>(g_ru_got.load()), static_cast<long>(g_ru_unmap.load()),
                  static_cast<long>(g_ru_sent.load()));
        TAP_CHECK(g_ru_unmap.load() == 0);
        TAP_CHECK(g_ru_got.load() == -KOS_EFAULT);
        TAP_CHECK(g_ru_sent.load() == -KOS_EFAULT);
    }

    // Frame-run creation after slot reuse, when a raw index no longer resolves as a generational
    // handle: more create/close cycles than the discovered capacity force the reuse.
    constexpr uint32_t FR_HOLD_MAX = 24;
    constexpr uint32_t FR_CYCLES = FR_HOLD_MAX;
    kos_cap_t g_fr_f[FR_HOLD_MAX];
    kos_cap_t g_fr_a[FR_HOLD_MAX];

    void t_frame_run_slot_recycle()
    {
        // Phase one: hold until a seed is refused.
        uint32_t held = 0;
        while (held < FR_HOLD_MAX)
        {
            uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0);
            if (seed == 0)
            {
                break;
            }
            g_fr_f[held] = static_cast<kos_cap_t>(seed & 0xFFFFFFFFull);
            g_fr_a[held] = static_cast<kos_cap_t>(seed >> 32);
            held++;
        }
        for (uint32_t i = 0; i < held; i++)
        {
            // Closing the last capability frees frames and advances the slot generation.
            (void)kos_handle_close(g_fr_f[i]);
            (void)kos_handle_close(g_fr_a[i]);
        }

        // Phase two: more cycles than phase one could hold at once.
        uint32_t seeded = 0;
        uint32_t mapped = 0;
        for (uint32_t i = 0; i < FR_CYCLES; i++)
        {
            uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0);
            if (seed == 0)
            {
                break;
            }
            seeded++;
            kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFull);
            kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
            uintptr_t const va = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 0);
            // Mapping must resolve the run through its current generational handle.
            if (va != 0 and kos_frame_map(fcap, acap, va, 0) == 0)
            {
                mapped++;
                (void)kos_frame_unmap(fcap, acap, va);
            }
            (void)kos_handle_close(fcap);
            (void)kos_handle_close(acap);
        }
        tap::diag("frame run recycle: %u held at once, then %u of %u cycle(s), %u mapped",
                  static_cast<unsigned>(held), static_cast<unsigned>(seeded),
                  static_cast<unsigned>(FR_CYCLES), static_cast<unsigned>(mapped));
        if (held == 0)
        {
            tap::skip("no frame capability to seed"); // 0 and not KOS_CAP_NONE: see the op
            return;
        }
        // Exhaustion, then enough cycles to prove slot reuse.
        TAP_CHECK(held < FR_HOLD_MAX);
        TAP_CHECK(FR_CYCLES > held);
        TAP_CHECK(seeded == FR_CYCLES);
        TAP_CHECK(mapped == seeded);
    }

    // A lower-priority witness on the subject's core runs only once the subject parked; call it
    // after the subject is runnable. False is a failed observation, not a pass.
    bool await_pinned_park()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        if (not kos::thread::create_caps(pool_probe_worker, nullptr, "parkw", TAP_PRIO_AFTER,
                                         caps, 1, KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                         nullptr, KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE)
                    .valid())
        {
            return false;
        }
        wait_n(1);
        return true;
    }

    // A failed reply-cap write-back revokes the cap and returns EFAULT to both ends. Site A uses
    // an already-unmapped opts page (boundary validation); site B unmaps after parking (rollback
    // after a successful validation). KICKOS_CAP_REPLY_MAX failures precede the control, so it
    // sees both the slot and the reply-cap budget recovered.
    constexpr size_t LU_LEN = 8;
    constexpr uint32_t LU_US = 2u * 1000u * 1000u;
    constexpr int CH_LU_EP = 2;
    constexpr int CH_LU_GATE = 3;
    kos_cap_t g_lu_gate = KOS_CAP_NONE;
    char g_lu_req[LU_LEN];
    char g_lu_rx[LU_LEN];
    uintptr_t g_lu_info = 0;
    Atomic<int32_t, Order::RELAXED> g_lu_call{99};
    Atomic<int32_t, Order::RELAXED> g_lu_reply{-1};
    Atomic<uint32_t, Order::RELAXED> g_lu_rounds{0};
    int32_t g_lu_got[KICKOS_CAP_REPLY_MAX + 1];

    // Parked on send_waiters before main receives, which reaches the receiver scan instead of
    // the fastpath.
    void lu_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        g_lu_call = kos_call_timed(CH_LU_EP, g_lu_req, LU_LEN, LU_LEN, LU_US);
        kos_sem_post(CH_DONE);
    }

    // One receiver across the failures and the control, so they spend one cap budget. main maps
    // opts before each gate and unmaps it only once the receiver parked, so the failure is at
    // delivery and not at syscall entry.
    void lu_receiver(void*) // caps: done@1, E(WAIT)@2, gate@3
    {
        for (uint32_t i = 0; i <= KICKOS_CAP_REPLY_MAX; i++)
        {
            kos_sem_wait(CH_LU_GATE);
            // Place opts on the mapped page so unmapping invalidates the reply-cap output.
            struct kos_reply_recv_opts* const o =
                reinterpret_cast<struct kos_reply_recv_opts*>(g_lu_info);
            kos_reply_recv_opts_init(o, CH_LU_EP, 0, KOS_TIMEOUT_NONE);
            int32_t const got =
                kos_reply_recv(KOS_CAP_NONE, g_lu_rx, kos_call_lens_pack(0, LU_LEN), o);
            g_lu_got[i] = got;
            g_lu_rounds = i + 1u;
            // Only in the round main left mapped, and never decided by got: a wrong success
            // would fault.
            if (i == KICKOS_CAP_REPLY_MAX)
            {
                g_lu_reply = kos_reply(o->info.reply_cap, g_lu_rx, LU_LEN);
            }
        }
        kos_sem_post(CH_DONE);
    }

    void t_call_reply_undisclosed()
    {
        uint64_t const seed = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0);
        if (seed == 0)
        {
            tap::skip("no frame capability to seed"); // 0 and not KOS_CAP_NONE: see the op
            return;
        }
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFull);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        uintptr_t const va = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 0);
        kos_cap_t ep = KOS_CAP_NONE;
        if (va == 0 or kos_endpoint_create(&ep) != 0)
        {
            (void)kos_handle_close(fcap);
            (void)kos_handle_close(acap);
            tap::skip("no seed window or no endpoint slot");
            return;
        }
        // The page must map and unmap before its unmapped state means anything.
        (void)kos_frame_unmap(fcap, acap, va);
        TAP_CHECK(kos_frame_map(fcap, acap, va, 0) == 0);
        TAP_CHECK(kos_frame_unmap(fcap, acap, va) == 0);
        g_lu_info = va;
        for (size_t i = 0; i < LU_LEN; i++)
        {
            g_lu_req[i] = static_cast<char>('a' + i);
        }
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_SIGNAL}};

        // Site A validates and writes the output under one lock without parking, so only boundary
        // validation is reachable; site B and tests/unit/capprobe cover capability rollback.
        bool spawned = true;
        int32_t const a_entry =
            kos_reply_recv(KOS_CAP_NONE, g_lu_rx, kos_call_lens_pack(0, LU_LEN),
                           reinterpret_cast<struct kos_reply_recv_opts*>(va));

        // Control: the same receiver scan with a valid output page.
        struct kos_recv_info a_info = {};
        a_info.reply_cap = KOS_CAP_NONE;
        int32_t a_ctl = -1;
        int a_reply = -1;
        g_lu_call = 99;
        if (kos::thread::create_caps(lu_caller, nullptr, "luctl", TAP_PRIO_PARKS, scaps, 2,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                     KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE)
                .valid())
        {
            if (await_pinned_park())
            {
                struct kos_reply_recv_opts opts;
                kos_reply_recv_opts_init(&opts, ep, 0, KOS_TIMEOUT_NONE);
                a_ctl = kos_reply_recv(KOS_CAP_NONE, g_lu_rx, kos_call_lens_pack(0, LU_LEN), &opts);
                a_info = opts.info;
                if (a_info.reply_cap != KOS_CAP_NONE)
                {
                    a_reply = kos_reply(a_info.reply_cap, g_lu_rx, LU_LEN);
                }
            }
            else
            {
                spawned = false; // no slot to read the park with, so nothing below is ordered
            }
            wait_n(1);
        }
        else
        {
            spawned = false;
        }

        // Site B: endpoint_call's fastpath, minting into a PARKED RECEIVER's table.
        g_lu_rounds = 0;
        g_lu_reply = -1;
        for (uint32_t i = 0; i <= KICKOS_CAP_REPLY_MAX; i++)
        {
            g_lu_got[i] = 99;
        }
        bool b_call_faulted = true;
        bool b_mapped = true;
        bool b_ordered = true;
        int32_t b_ctl = -1;
        int32_t b_call_saw = 77;
        if (spawned and kos_sem_create(0, &g_lu_gate) == 0)
        {
            kos_cap_grant rcaps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_WAIT},
                                     {g_lu_gate, CH_FULL}};
            if (kos::thread::create_caps(lu_receiver, nullptr, "lurcv", TAP_PRIO_PARKS, rcaps, 3,
                                         KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                         KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE)
                    .valid())
            {
                for (uint32_t i = 0; i < KICKOS_CAP_REPLY_MAX; i++)
                {
                    if (kos_frame_map(fcap, acap, va, 0) != 0)
                    {
                        b_mapped = false;
                        break;
                    }
                    kos_sem_post(g_lu_gate);
                    if (not await_pinned_park())
                    {
                        b_ordered = false;
                        break;
                    }
                    if (kos_frame_unmap(fcap, acap, va) != 0)
                    {
                        b_mapped = false;
                        break;
                    }
                    b_call_saw = kos_call_timed(ep, g_lu_req, LU_LEN, LU_LEN, LU_US);
                    if (b_call_saw != -KOS_EFAULT)
                    {
                        b_call_faulted = false;
                    }
                    // The receiver outranks main and must already have recorded this round.
                    if (g_lu_rounds.load() != i + 1u)
                    {
                        break;
                    }
                }
                // Mapped this time: the control needs the budget the failed mints returned.
                if (b_mapped and b_ordered)
                {
                    if (kos_frame_map(fcap, acap, va, 0) != 0)
                    {
                        b_mapped = false;
                    }
                    else
                    {
                        kos_sem_post(g_lu_gate);
                        if (await_pinned_park())
                        {
                            b_ctl = kos_call_timed(ep, g_lu_req, LU_LEN, LU_LEN, LU_US);
                        }
                        else
                        {
                            b_ordered = false;
                        }
                    }
                }
            }
            else
            {
                spawned = false;
            }
        }
        else
        {
            spawned = false;
        }
        bool b_recv_faulted = true;
        for (uint32_t i = 0; i < KICKOS_CAP_REPLY_MAX; i++)
        {
            if (g_lu_got[i] != -KOS_EFAULT)
            {
                b_recv_faulted = false;
            }
        }

        // The receiver stores its reply's result after that reply has released main.
        if (g_lu_rounds.load() == KICKOS_CAP_REPLY_MAX + 1u)
        {
            wait_n(1);
        }
        (void)kos_frame_unmap(fcap, acap, va);
        if (g_lu_gate != KOS_CAP_NONE)
        {
            kos_sem_destroy(g_lu_gate);
            g_lu_gate = KOS_CAP_NONE;
        }
        (void)kos_handle_close(ep);
        (void)kos_handle_close(fcap);
        (void)kos_handle_close(acap);
        tap::diag("local undisclosed: recv boundary %ld, recv control %ld replied %d; %u "
                  "fastpath refusal(s), call control %ld, receiver round(s) %u last %ld "
                  "replied %ld, call saw %ld", static_cast<long>(a_entry),
                  static_cast<long>(a_ctl), a_reply,
                  static_cast<unsigned>(KICKOS_CAP_REPLY_MAX), static_cast<long>(b_ctl),
                  static_cast<unsigned>(g_lu_rounds.load()),
                  static_cast<long>(g_lu_got[KICKOS_CAP_REPLY_MAX]),
                  static_cast<long>(g_lu_reply.load()), static_cast<long>(b_call_saw));
        if (not spawned or not b_ordered)
        {
            tap::skip("thread pool too small");
            return;
        }
        // Site A: an unmapped out-pointer never reaches the mint, and the ordinary
        // rendezvous through that same arm still lands.
        TAP_CHECK(a_entry == -KOS_EFAULT);
        TAP_CHECK(a_ctl == static_cast<int32_t>(LU_LEN));
        TAP_CHECK(a_info.reply_cap != KOS_CAP_NONE);
        TAP_CHECK(a_reply == 0);
        TAP_CHECK(g_lu_call.load() == static_cast<int32_t>(LU_LEN));
        // Site B: the same, with the mint on the other table.
        TAP_CHECK(b_mapped);
        TAP_CHECK(g_lu_rounds.load() == KICKOS_CAP_REPLY_MAX + 1u);
        TAP_CHECK(b_recv_faulted);
        TAP_CHECK(b_call_faulted);
        TAP_CHECK(g_lu_got[KICKOS_CAP_REPLY_MAX] == static_cast<int32_t>(LU_LEN));
        TAP_CHECK(g_lu_reply.load() == 0);
        TAP_CHECK(b_ctl == static_cast<int32_t>(LU_LEN));
    }

    void t_process_data_template()
    {
        settle_exits();
        constexpr uint32_t DT_BLK = 8u * DT_WORDS;
        void* const blk = kos_ram_alloc(DT_BLK);
        if (blk == nullptr)
        {
            tap::skip("arena cannot spare a report block");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(blk, DT_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_dt_word);
        uint64_t const rootf = kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, own);
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        out[DT_VALUE] = 0;
        out[DT_FRAME] = 0;
        kos_task_t t1 = KOS_TASK_NONE;
        if (kos_task_create(blk, DT_BLK, 0, &t1) != 0)
        {
            tap::skip("task pool too small");
            return;
        }
        if (not kos::thread::create_caps(dt_reader, blk, "dtA", 10, caps, 1, KOS_POLICY_FIFO, 0,
                                         false, nullptr, 0, 0, nullptr, t1).valid())
        {
            (void)kos_task_kill(t1);
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        (void)kos_task_kill(t1);
        uint64_t const first_value = out[DT_VALUE];
        uint64_t const first_frame = out[DT_FRAME];
        // Once main changes its globals, a new process must still read the saved snapshot.
        (void)kos_aspace_probe(KOS_ASPACE_OP_DATA_HOME_FORGET, 0);
        g_dt_word = DT_B;
        out[DT_VALUE] = 0;
        out[DT_FRAME] = 0;
        kos_task_t t2 = KOS_TASK_NONE;
        if (kos_task_create(blk, DT_BLK, 0, &t2) != 0)
        {
            g_dt_word = DT_A;
            tap::skip("task pool too small for the second process");
            return;
        }
        if (not kos::thread::create_caps(dt_reader, blk, "dtB", 10, caps, 1, KOS_POLICY_FIFO, 0,
                                         false, nullptr, 0, 0, nullptr, t2).valid())
        {
            (void)kos_task_kill(t2);
            g_dt_word = DT_A;
            tap::skip("thread pool too small for the second process");
            return;
        }
        wait_n(1);
        (void)kos_task_kill(t2);
        uint64_t const late_value = out[DT_VALUE];
        uint64_t const late_frame = out[DT_FRAME];
        g_dt_word = DT_A;
        tap::diag("data template: main frame %u, first %u/%u, after the home is lost %u/%u",
                  static_cast<unsigned>(rootf), static_cast<unsigned>(first_frame),
                  static_cast<unsigned>(first_value), static_cast<unsigned>(late_frame),
                  static_cast<unsigned>(late_value));
        TAP_CHECK(rootf != 0 and first_frame != 0 and late_frame != 0);
        TAP_CHECK(first_value == DT_A and first_frame != rootf);
        TAP_CHECK(late_value == DT_A); // the snapshot, not main's live word
        TAP_CHECK(late_frame != rootf); // a frame of its own, not the image's own page
        settle_exits();
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // A task's static data is the image's as root held it at the first explicit task, the init
    // creating main's, never a global main wrote since; the task after the write and its restart
    // both read the image's value. Before process_data_template, which forgets the image's data
    // home for good.
    void t_process_data_from_image()
    {
        settle_exits();
        constexpr uint32_t FI_BLK = 8u * DT_WORDS;
        void* const blk = kos_ram_alloc(FI_BLK);
        if (blk == nullptr)
        {
            tap::skip("arena cannot spare a report block");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(blk, FI_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        kos_task_t first = KOS_TASK_NONE;
        if (kos_task_create(blk, FI_BLK, 0, &first) != 0)
        {
            tap::skip("task pool too small");
            return;
        }
        (void)kos_task_kill(first);
        g_dt_word = DT_B;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        uint64_t seen[2] = {0, 0};
        int ran = 0;
        for (int run = 0; run < 2; run++)
        {
            out[DT_VALUE] = 0;
            kos_task_t t = KOS_TASK_NONE;
            if (kos_task_create(blk, FI_BLK, 0, &t) != 0)
            {
                break;
            }
            if (not kos::thread::create_caps(dt_reader, blk, "fiR", 10, caps, 1, KOS_POLICY_FIFO,
                                             0, false, nullptr, 0, 0, nullptr, t).valid())
            {
                (void)kos_task_kill(t);
                break;
            }
            wait_n(1);
            (void)kos_task_kill(t);
            seen[run] = out[DT_VALUE];
            ran++;
        }
        g_dt_word = DT_A;
        settle_exits();
        if (ran != 2)
        {
            tap::skip("task or thread pool too small for the restart");
            return;
        }
        tap::diag("data from the image: first %u, restart %u",
                  static_cast<unsigned>(seen[0]), static_cast<unsigned>(seen[1]));
        TAP_CHECK(seen[0] == DT_A); // not main's DT_B
        TAP_CHECK(seen[1] == DT_A);
    }

    // Space-less threads must not write the app's reentrancy slots through a previously active
    // process mapping. Bit zero is the positive control that the seating case ran.
    void t_reent_seating()
    {
        kos_sleep_ns(2000000ull); // an idle window inside this arm, not only in an earlier one
        uint64_t const v = kos_aspace_probe(KOS_ASPACE_OP_REENT_SEATING, 0);
        tap::diag("reent seating word %u", static_cast<unsigned>(v));
        TAP_CHECK((v & 1u) != 0);
        TAP_CHECK((v & 2u) == 0);
    }

    // One release per successful acquire. The counter sees a mismatch even on direct-map
    // backends whose release needs no window update.
    void t_aspace_acquire_balance()
    {
        settle_exits();
        // Page zero, which no space maps: the frame-token pair's second acquire answers null
        // and its first does not.
        uint64_t const unmapped = kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, 0);
        uint64_t const bal = kos_aspace_probe(KOS_ASPACE_OP_ACQUIRE_BALANCE, 0);
        tap::diag("token for an unmapped page %u, acquire balance %u live / %u unpaired",
                  static_cast<unsigned>(unmapped), static_cast<unsigned>(bal >> 32),
                  static_cast<unsigned>(bal & 0xFFFFFFFFu));
        TAP_CHECK(unmapped == 0);
        TAP_CHECK(bal == 0);
    }

    // Seeding a never-run space needs no invalidation. Widening the running space
    // must still perform maintenance.
    void t_map_tlbi_elided()
    {
        constexpr uint32_t TLBI_BLK = 256;
        uint64_t const before = kos_aspace_probe(KOS_ASPACE_OP_MAP_TLBI, 0);
        // The low byte counts invalid releases since boot; checked before any skip so every
        // configuration validates it.
        TAP_CHECK((before & 0xFFu) == 0);
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            tap::skip("task or domain pool too small for one more space");
            return;
        }
        uint64_t const after = kos_aspace_probe(KOS_ASPACE_OP_MAP_TLBI, 0);
        (void)kos_task_kill(t);
        uint32_t const issued = static_cast<uint32_t>((after >> 32) - (before >> 32));
        uint32_t const elided = static_cast<uint32_t>(((after >> 8) & 0xFFFFFFu)
                                                      - ((before >> 8) & 0xFFFFFFu));
        TAP_CHECK((after & 0xFFu) == 0);
        tap::diag("image seed: %u sequences issued, %u elided", issued, elided);
        // No core is resident in a never-activated space, regardless of core count.
        TAP_CHECK(issued == 0);
        // A seed reporting a handful mapped almost none of the image.
        TAP_CHECK(elided >= 32u);
        // Control: the running space needs invalidation.
        void* const blk = kos_ram_alloc(TLBI_BLK);
        if (blk == nullptr)
        {
            tap::partial("no reservation left for the installed-space control");
            return;
        }
        uint64_t const pre = kos_aspace_probe(KOS_ASPACE_OP_MAP_TLBI, 0);
        TAP_CHECK(kos_mem_self_grant(blk, TLBI_BLK, 0) == 0);
        uint64_t const post = kos_aspace_probe(KOS_ASPACE_OP_MAP_TLBI, 0);
        tap::diag("running-space widening: %u sequences issued",
                  static_cast<unsigned>((post >> 32) - (pre >> 32)));
        TAP_CHECK((post >> 32) > (pre >> 32));
    }

#if KICKOS_KERNEL_CORES > 1 && defined(__x86_64__)
    // Translation rendezvous initiated by every core since boot, which on this backend are the
    // shootdown round trips.
    uint64_t rendezvous_total()
    {
        uint64_t const width = kos_doorbell_probe(KOS_DOORBELL_OP_WIDTH, 0);
        uint64_t sum = 0;
        for (uint64_t c = 0; c < width; c++)
        {
            sum += kos_doorbell_probe(KOS_DOORBELL_OP_COUNTS, static_cast<uint32_t>(c)) >> 32;
        }
        return sum;
    }

    void rendezvous_parker(void*) // caps: ready@1, park@2
    {
        kos_sem_post(KOS_SPAWN_DELEGATED_CAP0);
        kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0 + 1);
        kos_exit(0);
    }

    // The round trips a mapping change makes to the other cores do not grow with its page count:
    // a grant of many granules costs what a grant of one does, and a thread's exit, which
    // unmaps its whole stack, costs fewer than that stack has granules. The counters sum every
    // core, so no other thread may change a mapping inside a sample.
    void t_shootdown_per_change()
    {
        uintptr_t const g = static_cast<uintptr_t>(kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0));
        constexpr uint32_t MANY = 8;
        void* const one = kos_ram_alloc(g);
        void* const many = kos_ram_alloc(g * MANY);
        if (one == nullptr or many == nullptr)
        {
            tap::skip("no reservation left for the two grants");
            return;
        }
        settle_exits();
        uint64_t const t0 = rendezvous_total();
        TAP_CHECK(kos_mem_self_grant(one, g, 0) == 0);
        uint64_t const t1 = rendezvous_total();
        TAP_CHECK(kos_mem_self_grant(many, g * MANY, 0) == 0);
        uint64_t const t2 = rendezvous_total();
        uint32_t const for_one = static_cast<uint32_t>(t1 - t0);
        uint32_t const for_many = static_cast<uint32_t>(t2 - t1);

        kos_cap_t ready = KOS_CAP_NONE;
        kos_cap_t park = KOS_CAP_NONE;
        kos_task_t task = KOS_TASK_NONE;
        if (kos_sem_create(0, &ready) != 0 or kos_sem_create(0, &park) != 0
            or kos_task_create(nullptr, 0, 0, &task) != 0)
        {
            if (ready != KOS_CAP_NONE)
            {
                (void)kos_handle_close(ready);
            }
            if (park != KOS_CAP_NONE)
            {
                (void)kos_handle_close(park);
            }
            tap::partial("no semaphore or task for the exit half");
            return;
        }
        kos_cap_grant const caps[2] = {{ready, KOS_CAP_SIGNAL}, {park, KOS_CAP_WAIT}};
        auto const parker = kos::thread::create(rendezvous_parker, nullptr, "rvx", 10,
                                                KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                nullptr, 0, nullptr, 0, nullptr, 0, caps, 2,
                                                /*authority=*/0, /*cap_dest=*/nullptr, task);
        if (not parker.valid())
        {
            (void)kos_task_kill(task);
            (void)kos_handle_close(ready);
            (void)kos_handle_close(park);
            tap::partial("pool too small for the exiting thread");
            return;
        }
        kos_sem_wait(ready);
        uint64_t const e0 = rendezvous_total();
        kos_sem_post(park);
        int const joined = parker.join(STALL_TOLERANT_US);
        settle_exits();
        uint64_t const e1 = rendezvous_total();
        TAP_CHECK(kos_task_kill(task) == 0);
        TAP_CHECK(kos_handle_close(ready) == 0);
        TAP_CHECK(kos_handle_close(park) == 0);
        uint32_t const for_exit = static_cast<uint32_t>(e1 - e0);
        uint32_t const stack_granules = static_cast<uint32_t>(KICKOS_SELFTEST_USER_STACK_SIZE / g);
        tap::diag("round trips: grant of 1 granule %u, of %u granules %u, thread exit %u "
                  "(stack of %u granules)",
                  for_one, MANY, for_many, for_exit, stack_granules);
        TAP_CHECK(joined == 0);
        TAP_CHECK(for_one > 0 and for_many == for_one);
        TAP_CHECK(for_exit > 0 and for_exit < stack_granules);
    }
#endif

    // Every installed root must be the boot root or belong to a live domain. A thread without a
    // space must restore the boot root before the old process tables can be freed.
    void t_aspace_active_cores()
    {
        uint64_t const w = kos_aspace_probe(KOS_ASPACE_OP_ACTIVE_CORES, 0);
        unsigned const cores = static_cast<unsigned>((w >> 16) & 0xFFu);
        unsigned const accounted = static_cast<unsigned>((w >> 8) & 0xFFu);
        unsigned const mine = static_cast<unsigned>(w & 0xFFu);
        tap::diag("%u kernel core(s), %u on a live root, this task's space on %u", cores,
                  accounted, mine);
        // The denominator, without which the equality below is satisfied by an empty set.
        TAP_CHECK(cores == static_cast<unsigned>(KICKOS_KERNEL_CORES));
        TAP_CHECK(mine >= 1u);
        TAP_CHECK(accounted == cores);

        // After task churn, no core may retain a destroyed root.
        for (unsigned i = 0; i < 4u; i++)
        {
            kos_task_t t = KOS_TASK_NONE;
            if (kos_task_create(nullptr, 0, 0, &t) != 0)
            {
                break;
            }
            kos_yield();
            (void)kos_task_kill(t);
            kos_yield();
        }
        uint64_t const after = kos_aspace_probe(KOS_ASPACE_OP_ACTIVE_CORES, 0);
        unsigned const cores_after = static_cast<unsigned>((after >> 16) & 0xFFu);
        unsigned const accounted_after = static_cast<unsigned>((after >> 8) & 0xFFu);
        tap::diag("after the kill churn: %u of %u core(s) on a live root", accounted_after,
                  cores_after);
        TAP_CHECK(cores_after == static_cast<unsigned>(KICKOS_KERNEL_CORES));
        TAP_CHECK(accounted_after == cores_after);
    }

    // Absolute words the link stores in the app's own data. Volatile, so each is read from
    // memory and not folded from its initializer.
    uint32_t g_reloc_target = 0x5eedu;
    uint32_t* volatile g_reloc_data_ptr = &g_reloc_target;
    __attribute__((noinline)) uint32_t reloc_text_target()
    {
        return g_reloc_target + 1u;
    }
    uint32_t (*volatile g_reloc_text_ptr)() = &reloc_text_target;
    uint32_t volatile g_ctor_ran = 0;
    struct CtorWitness
    {
        CtorWitness()
        {
            g_ctor_ran = 1;
        }
    };
    CtorWitness g_ctor_witness;

    // The app's initialized pointers and constructor table hold link-time addresses the load
    // relocated. Each must equal the address the running code computes for the same object,
    // which on x86_64 is the app window's alias and not the kernel's view of it.
    void t_app_pointers_relocated()
    {
        uint32_t* const data = g_reloc_data_ptr;
        uint32_t (*const text)() = g_reloc_text_ptr;
        tap::diag("data word 0x%lx, text word 0x%lx",
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(data)),
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(text)));
        TAP_CHECK(g_ctor_ran == 1u);
        TAP_CHECK(data == &g_reloc_target);
        TAP_CHECK(text == &reloc_text_target);
        if (data == &g_reloc_target and text == &reloc_text_target)
        {
            TAP_CHECK(*data == 0x5eedu);
            TAP_CHECK(text() == 0x5eeeu);
        }
    }
#endif
}
