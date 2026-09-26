// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The multi-core arms: placement, slice preemption, cross-core wakes and IRQs, and libc state.

#include "selftest.h"

#include <errno.h>
#include <stdlib.h>

namespace selftest
{
#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
    // --- Placement: affinity, the task's core set, and the isolated cores -----------------
    // Every scheduling probe reads the CALLER's state: a worker reports what the kernel seated
    // on it, not what root asked for.
    constexpr uint64_t PLACE_STEP_NS = 500000ull;
    constexpr unsigned PL_SAMPLES = 64;
    constexpr unsigned PL_DEADLINE_STEPS = 400; // 200 ms
    constexpr uint32_t PL_ALL = ~0u >> (32 - KICKOS_KERNEL_CORES);

    kos_cap_t g_pl_gate = KOS_CAP_NONE;

    // Keeps its task non-empty until root posts the gate.
    void pl_gate_worker(void*) // caps: gate@1
    {
        kos_sem_wait(1);
    }

    Atomic<uint32_t, Order::RELAXED> g_pl_go{0};
    Atomic<uint32_t, Order::RELAXED> g_pl_stop{0};
    Atomic<uint32_t, Order::RELAXED> g_pl_core{0xffu};
    Atomic<uint32_t, Order::RELAXED> g_pl_seen{0};
    Atomic<uint32_t, Order::RELAXED> g_pl_aff{0};
    Atomic<uint32_t, Order::RELAXED> g_pl_cores{0};

    void pl_reset()
    {
        g_pl_go = 0;
        g_pl_stop = 0;
        g_pl_core = 0xffu;
        g_pl_seen = 0;
        g_pl_aff = 0;
        g_pl_cores = 0;
    }

    // The gate SLEEPS rather than yields: a worker above root's priority that spun here would
    // hold the core root has to run on to release it.
    void pl_wait_go()
    {
        while (g_pl_go.load() == 0)
        {
            kos_sleep_ns(PLACE_STEP_NS);
        }
    }

    void pl_sample_worker(void*)
    {
        pl_wait_go();
        g_pl_aff = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_AFFINITY));
        g_pl_cores = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_TASK_CORES));
        uint32_t seen = 0;
        for (unsigned i = 0; i < PL_SAMPLES; i++)
        {
            uint32_t const c = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_CORE));
            g_pl_core = c;
            seen |= 1u << c;
            kos_yield();
        }
        g_pl_seen = seen;
    }

    void pl_park_worker(void*)
    {
        while (g_pl_stop.load() == 0)
        {
            kos_sleep_ns(PLACE_STEP_NS);
        }
    }

    // Runnable throughout, never parked: the migration arm needs a target the kernel has to
    // take off a core rather than one it can simply place at its next wake.
    void pl_spin_worker(void*)
    {
        // An UNPINNED spinner can land on the core a pin would have named, so g_pl_core alone
        // cannot tell a pin that took from one never made.
        g_pl_aff = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_AFFINITY));
        while (g_pl_stop.load() == 0)
        {
            g_pl_core = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_CORE));
        }
    }

    uint32_t pl_lowest(uint32_t mask)
    {
        for (uint32_t c = 0; c < static_cast<uint32_t>(KICKOS_KERNEL_CORES); c++)
        {
            if ((mask & (1u << c)) != 0)
            {
                return c;
            }
        }
        return 0;
    }

    void t_pin_places()
    {
        pl_reset();
        auto w = kos::thread::create(pl_sample_worker, nullptr, "pinpl", 12);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        int const rc = kos::thread::pin(w.id(), 1);
        g_pl_go = 1;
        int const joined = w.join();
        uint32_t const aff = g_pl_aff;
        uint32_t const core = g_pl_core;
        tap::diag("pinned to core 1: affinity 0x%x, running on core %u",
                  static_cast<unsigned>(aff), static_cast<unsigned>(core));
        TAP_CHECK(rc == 0);
        TAP_CHECK(joined == 0);
        TAP_CHECK(aff == (1u << 1));
        TAP_CHECK(core == 1u);
    }

    void t_pin_wrong_core_never()
    {
        pl_reset();
        auto w = kos::thread::create(pl_sample_worker, nullptr, "pinnv", 12);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        int const rc = kos::thread::pin(w.id(), 1);
        g_pl_go = 1;
        int const joined = w.join();
        uint32_t const seen = g_pl_seen;
        tap::diag("%u samples across a yield each: cores 0x%x", PL_SAMPLES,
                  static_cast<unsigned>(seen));
        TAP_CHECK(rc == 0);
        TAP_CHECK(joined == 0);
        TAP_CHECK(seen == (1u << 1));
    }

    void t_unpin_restores()
    {
        pl_reset();
        auto w = kos::thread::create_caps(pl_sample_worker, nullptr, "unpin", 12, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, 1u << 1);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        uint32_t const iso = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_ISOLATED));
        int const rc = kos::thread::unpin(w.id());
        g_pl_go = 1;
        int const joined = w.join();
        uint32_t const aff = g_pl_aff;
        uint32_t const cores = g_pl_cores;
        tap::diag("unpin returned %d: affinity 0x%x, task core set 0x%x, isolated 0x%x", rc,
                  static_cast<unsigned>(aff), static_cast<unsigned>(cores),
                  static_cast<unsigned>(iso));
        TAP_CHECK(joined == 0);
        TAP_CHECK(cores == PL_ALL);
        // Unpin is a mask of zero, resolved to the task's DEFAULT set, so it cannot be refused.
        // The default and not the grant: the grant names the isolated cores, which a thread that
        // never named one must not gain.
        TAP_CHECK(rc == 0);
        TAP_CHECK(aff == (cores & ~iso));
    }

    enum
    {
        XS_CORES = 0, // the member's own task core set
        XS_MADE = 1,  // it got a sibling to place at all
        XS_ONE = 2,   // placing that sibling on core 1
        XS_ZERO = 3,  // ... and on core 0
        XS_WORDS = 4
    };
    // Pins a SIBLING in its own task: the task's single grant bounds the placement.
    void sib_pin_worker(void*) // caps: E(SIGNAL)@1
    {
        int32_t rep[XS_WORDS] = {0, 0, -99, -99};
        rep[XS_CORES] = static_cast<int32_t>(kos_sched_probe(KOS_SCHED_OP_TASK_CORES));
        g_pl_stop = 0;
        auto s = kos::thread::create(pl_park_worker, nullptr, "sibpk", 9);
        if (s.valid())
        {
            rep[XS_MADE] = 1;
            rep[XS_ONE] = kos::thread::pin(s.id(), 1);
            rep[XS_ZERO] = kos::thread::pin(s.id(), 0);
            g_pl_stop = 1;
            (void)s.join();
        }
        (void)kos_send(1, rep, sizeof(rep));
    }

    // `t` is KOS_TASK_NONE for root's own task. False when the spawn or the rendezvous failed.
    bool sib_pin_run(kos_task_t t, int32_t* rep)
    {
        kos_cap_grant caps[] = {{g_pl_ep, KOS_CAP_SIGNAL}};
        auto m = kos::thread::create_caps(sib_pin_worker, nullptr, "sibpn", 11, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          t);
        if (not m.valid())
        {
            return false;
        }
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, g_pl_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        bool const heard = kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(int32_t) * XS_WORDS), &opts)
                           == static_cast<int32_t>(sizeof(int32_t) * XS_WORDS);
        bool const joined = m.join() == 0;
        return heard and joined;
    }

    void t_affinity_zero_defaults()
    {
        pl_reset();
        auto w = kos::thread::create(pl_park_worker, nullptr, "azero", 9);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        // A mask of zero asks for the task's DEFAULT set, as a spawn's zero core_mask does; it is
        // not malformed.
        int const zero = kos_thread_set_affinity(w.id(), 0);
        g_pl_stop = 1;
        int const joined = w.join();
        TAP_CHECK(joined == 0);
        TAP_CHECK(zero == 0);

        // The authority refusal: the task holds core 0 alone, so a pin to core 1, a core the
        // image drives, is refused.
        if (kos_endpoint_create(&g_pl_ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            (void)kos_handle_close(g_pl_ep);
            tap::skip("task pool too small");
            return;
        }
        int const granted = kos_task_sched_grant(t, 0, 0x1);
        int32_t rep[XS_WORDS] = {0, 0, 0, 0};
        bool const ran = sib_pin_run(t, rep);
        (void)kos_task_kill(t);
        (void)kos_handle_close(g_pl_ep);
        TAP_CHECK(granted == 0);
        TAP_CHECK(ran);
        TAP_CHECK(rep[XS_CORES] == 0x1);
        TAP_CHECK(rep[XS_MADE] == 1);
        TAP_CHECK(rep[XS_ONE] == -KOS_EPERM);
        TAP_CHECK(rep[XS_ZERO] == 0);
    }

    void t_affinity_undriven_refused()
    {
        pl_reset();
        auto w = kos::thread::create(pl_park_worker, nullptr, "aundr", 9);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        uint32_t const undriven = 1u << static_cast<uint32_t>(KICKOS_KERNEL_CORES);
        int const bad = kos_thread_set_affinity(w.id(), undriven);
        // A mask is a set of acceptable cores: an undriven bit beside a driven one names the
        // driven one and is granted, so all ones is an ordinary request for the whole grant.
        int const mixed = kos_thread_set_affinity(w.id(), undriven | 0x1u);
        int const good = kos_thread_set_affinity(w.id(), 0x1u);
        g_pl_stop = 1;
        int const joined = w.join();
        TAP_CHECK(joined == 0);
        TAP_CHECK(bad == -KOS_EINVAL);
        TAP_CHECK(mixed == 0);
        TAP_CHECK(good == 0);
    }

    // --- Placement: a running thread is re-placed under an affinity change ---------------
    // A pinned child drives the move: root holds no handle to itself, so it cannot be kept off
    // the spinner's core. The spinner sits above root and the driver above the spinner, so the
    // driver stays scheduled once both share the destination.
    constexpr uint32_t PL_HOME = 0; // the boot core
    kos_thread_t g_pl_victim = KOS_THREAD_NONE;
    Atomic<uint32_t, Order::RELAXED> g_pl_mig_first{0xffu};
    Atomic<uint32_t, Order::RELAXED> g_pl_mig_last{0xffu};
    Atomic<uint32_t, Order::RELAXED> g_pl_mig_arrived{0};
    Atomic<uint32_t, Order::RELAXED> g_pl_mig_rc{0x7fffffffu};

    void pl_migrate_driver(void*)
    {
        pl_wait_go();
        // Wait for the spinner's first publish before moving it: pinned to the source at
        // creation, its first value is that core, so a slow start fails the precondition instead
        // of losing a race.
        uint32_t first = 0xffu;
        for (unsigned i = 0; i < PL_DEADLINE_STEPS; i++)
        {
            first = g_pl_core.load();
            if (first != 0xffu)
            {
                break;
            }
            kos_sleep_ns(PLACE_STEP_NS);
        }
        g_pl_mig_first = first;
        int const rc = kos::thread::pin(g_pl_victim, PL_HOME);
        g_pl_mig_rc = static_cast<uint32_t>(rc);
        uint32_t arrived = 0;
        for (unsigned i = 0; i < PL_DEADLINE_STEPS; i++)
        {
            if (g_pl_core.load() == PL_HOME)
            {
                arrived = 1;
                break;
            }
            kos_sleep_ns(PLACE_STEP_NS);
        }
        g_pl_mig_last = g_pl_core.load();
        g_pl_mig_arrived = arrived;
        g_pl_stop = 1;
    }

    void t_migrate_running()
    {
        uint32_t const iso = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_ISOLATED));
        // The boot core is the destination because it can never be isolated. The source must not
        // be isolated either: a move involving an isolated core is a different claim.
        uint32_t away = 0;
        unsigned found = 0;
        for (uint32_t c = 1; c < static_cast<uint32_t>(KICKOS_KERNEL_CORES); c++)
        {
            if ((iso & (1u << c)) != 0)
            {
                continue;
            }
            away = c;
            found = 1;
            break;
        }
        if (found == 0)
        {
            tap::skip("a migration needs a non-isolated core beside the boot core");
            return;
        }
        pl_reset();
        g_pl_mig_first = 0xffu;
        g_pl_mig_last = 0xffu;
        g_pl_mig_arrived = 0;
        g_pl_mig_rc = 0x7fffffffu;
        auto w = kos::thread::create_caps(pl_spin_worker, nullptr, "migr", 12, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, 1u << away);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        // Set before the driver exists, so the relaxed cell needs no publication order.
        g_pl_victim = w.id();
        auto d = kos::thread::create(pl_migrate_driver, nullptr, "migrd", 13);
        if (not d.valid())
        {
            g_pl_stop = 1;
            (void)w.join();
            tap::skip("thread pool too small");
            return;
        }
        int const dpin = kos::thread::pin(d.id(), PL_HOME);
        g_pl_go = 1;
        int const djoined = d.join();
        int const joined = w.join();
        uint32_t const first = g_pl_mig_first;
        uint32_t const last = g_pl_mig_last;
        uint32_t const aff = g_pl_aff;
        int const rc = static_cast<int>(g_pl_mig_rc.load());
        tap::diag("spinner core %u -> 0 under a driver pinned to 0: affinity 0x%x, first %u, "
                  "arrived %u, last read %u", static_cast<unsigned>(away),
                  static_cast<unsigned>(aff), static_cast<unsigned>(first),
                  static_cast<unsigned>(g_pl_mig_arrived.load()),
                  static_cast<unsigned>(last));
        TAP_CHECK(dpin == 0);
        TAP_CHECK(djoined == 0);
        TAP_CHECK(joined == 0);
        // The precondition: the spinner ran pinned to the source before the move.
        TAP_CHECK(aff == (1u << away));
        TAP_CHECK(first == away);
        TAP_CHECK(rc == 0);
        // A deadline and not a park, so a spinner that never arrives fails instead of hanging.
        TAP_CHECK(g_pl_mig_arrived.load() == 1u);
    }

    void t_pin_same_task_ok()
    {
        if (kos_endpoint_create(&g_pl_ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        pl_reset();
        int32_t rep[XS_WORDS] = {0, 0, 0, 0};
        bool const ran = sib_pin_run(KOS_TASK_NONE, rep);
        (void)kos_handle_close(g_pl_ep);
        TAP_CHECK(ran);
        TAP_CHECK(rep[XS_CORES] == static_cast<int32_t>(PL_ALL));
        TAP_CHECK(rep[XS_MADE] == 1);
        TAP_CHECK(rep[XS_ONE] == 0);
        TAP_CHECK(rep[XS_ZERO] == 0);
    }

    void t_pin_cross_task_refused()
    {
        if (kos_sem_create(0, &g_pl_gate) != 0)
        {
            tap::skip("semaphore pool too small");
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            (void)kos_handle_close(g_pl_gate);
            tap::skip("task pool too small");
            return;
        }
        kos_cap_grant caps[] = {{g_pl_gate, CH_FULL}};
        auto m = kos::thread::create_caps(pl_gate_worker, nullptr, "xtask", 9, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          t);
        bool const seated = m.valid();
        int rc = -99;
        int mj = -1;
        if (seated)
        {
            rc = kos_thread_set_affinity(m.id(), 0x1u);
            kos_sem_post(g_pl_gate);
            mj = m.join();
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(g_pl_gate);
        TAP_CHECK(seated);
        TAP_CHECK(mj == 0);
        // Root is unprivileged: a core its OWN grant holds does not make another task's thread
        // its to place.
        TAP_CHECK(rc == -KOS_EPERM);
    }

    enum
    {
        GN_CORES = 0, // the member's task core set
        GN_AFF = 1,   // the affinity the kernel seated on it from that set
        GN_CORE = 2,  // the core it was reading this on
        GN_WORDS = 3
    };
    void gn_worker(void*) // caps: E(SIGNAL)@1
    {
        int32_t rep[GN_WORDS];
        rep[GN_CORES] = static_cast<int32_t>(kos_sched_probe(KOS_SCHED_OP_TASK_CORES));
        rep[GN_AFF] = static_cast<int32_t>(kos_sched_probe(KOS_SCHED_OP_AFFINITY));
        rep[GN_CORE] = static_cast<int32_t>(kos_sched_probe(KOS_SCHED_OP_CORE));
        (void)kos_send(1, rep, sizeof(rep));
    }

    void t_grant_narrows()
    {
        if (kos_endpoint_create(&g_pl_ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            (void)kos_handle_close(g_pl_ep);
            tap::skip("task pool too small");
            return;
        }
        int const granted = kos_task_sched_grant(t, 0, 0x3);
        kos_cap_grant caps[] = {{g_pl_ep, KOS_CAP_SIGNAL}};
        auto m = kos::thread::create_caps(gn_worker, nullptr, "gnarr", 11, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          t);
        bool const seated = m.valid();
        int32_t rep[GN_WORDS] = {0, 0, 0};
        bool heard = false;
        int mj = -1;
        if (seated)
        {
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, g_pl_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
            heard = kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(rep)), &opts)
                    == static_cast<int32_t>(sizeof(rep));
            mj = m.join();
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(g_pl_ep);
        TAP_CHECK(granted == 0);
        TAP_CHECK(seated);
        TAP_CHECK(heard);
        TAP_CHECK(mj == 0);
        TAP_CHECK(rep[GN_CORES] == 0x3);
        // Affinity is seated FROM the set, so a grant nothing re-derived shows as a thread wider
        // than its own task.
        TAP_CHECK(rep[GN_AFF] == 0x3);
        TAP_CHECK((0x3 & (1 << rep[GN_CORE])) != 0);
    }

    enum
    {
        GW_CORES = 0,  // the member's task core set
        GW_MADE = 1,   // it got a task of its own to narrow
        GW_WIDER = 2,  // a set wider than that
        GW_WITHIN = 3, // ... and one inside it
        GW_WORDS = 4
    };
    void gw_worker(void*) // caps: E(SIGNAL)@1
    {
        int32_t rep[GW_WORDS] = {0, 0, -99, -99};
        rep[GW_CORES] = static_cast<int32_t>(kos_sched_probe(KOS_SCHED_OP_TASK_CORES));
        kos_task_t u = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &u) == 0)
        {
            rep[GW_MADE] = 1;
            rep[GW_WIDER] = kos_task_sched_grant(u, 0, 0x3);
            rep[GW_WITHIN] = kos_task_sched_grant(u, 0, 0x1);
            (void)kos_task_kill(u);
        }
        (void)kos_send(1, rep, sizeof(rep));
    }

    void t_grant_wider_refused()
    {
        if (kos_endpoint_create(&g_pl_ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            (void)kos_handle_close(g_pl_ep);
            tap::skip("task pool too small");
            return;
        }
        int const granted = kos_task_sched_grant(t, 0, 0x1);
        kos_cap_grant caps[] = {{g_pl_ep, KOS_CAP_SIGNAL}};
        auto m = kos::thread::create_caps(gw_worker, nullptr, "gwide", 11, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          t);
        bool const seated = m.valid();
        int32_t rep[GW_WORDS] = {0, 0, 0, 0};
        bool heard = false;
        int mj = -1;
        if (seated)
        {
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, g_pl_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
            heard = kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(rep)), &opts)
                    == static_cast<int32_t>(sizeof(rep));
            mj = m.join();
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(g_pl_ep);
        TAP_CHECK(granted == 0);
        TAP_CHECK(seated);
        TAP_CHECK(heard);
        TAP_CHECK(mj == 0);
        TAP_CHECK(rep[GW_CORES] == 0x1);
        TAP_CHECK(rep[GW_MADE] == 1);
        TAP_CHECK(rep[GW_WIDER] == -KOS_EPERM);
        TAP_CHECK(rep[GW_WITHIN] == 0);
    }

    // --- A second grant narrows again and never re-widens -------------------------------
    // task_sched_grant weighs a request against the CALLER's grant, which root holds whole, so
    // only task_sched_narrow's check against the TASK's current set can refuse this.
    void t_grant_second_narrow_only()
    {
        if (PL_ALL == 0x1u)
        {
            tap::skip("re-widening needs a set wider than core 0 to ask for");
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            tap::skip("task pool too small");
            return;
        }
        int const first = kos_task_sched_grant(t, 0, 0x1);
        int const rewiden = kos_task_sched_grant(t, 0, PL_ALL);
        int const again = kos_task_sched_grant(t, 0, 0x1);
        (void)kos_task_kill(t);
        TAP_CHECK(first == 0);
        TAP_CHECK(rewiden == -KOS_EPERM);
        TAP_CHECK(again == 0);
    }

    // --- A task inherits its creator's grant --------------------------------------------
    // Only a member of the created task can show this: every refusal path weighs the caller's
    // grant, so a task wrongly defaulted to the whole machine is invisible from outside.
    enum
    {
        GI_MADE = 0,   // the nested task was created
        GI_SEATED = 1, // a member of it was seated
        GI_CORES = 2,  // and the set that member reports
        GI_WORDS = 3
    };
    void gi_child(void*) // caps: E(SIGNAL)@1
    {
        int32_t rep[GI_WORDS] = {1, 1,
                                 static_cast<int32_t>(kos_sched_probe(KOS_SCHED_OP_TASK_CORES))};
        (void)kos_send(1, rep, sizeof(rep));
    }
    void gi_worker(void*) // caps: E(SIGNAL)@1
    {
        int32_t rep[GI_WORDS] = {0, 0, -1};
        kos_task_t u = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &u) != 0)
        {
            (void)kos_send(1, rep, sizeof(rep));
            return;
        }
        rep[GI_MADE] = 1;
        kos_cap_grant caps[] = {{1, KOS_CAP_SIGNAL}};
        auto c = kos::thread::create_caps(gi_child, nullptr, "ginh", 11, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr, u);
        if (not c.valid())
        {
            (void)kos_task_kill(u);
            (void)kos_send(1, rep, sizeof(rep));
            return;
        }
        // The CHILD answers on the same endpoint, so this thread sends nothing more.
        (void)c.join();
        (void)kos_task_kill(u);
    }
    void t_grant_inherited_by_child_task()
    {
        if (PL_ALL == 0x1u)
        {
            tap::skip("inheritance needs a set narrower than the whole machine to inherit");
            return;
        }
        if (kos_endpoint_create(&g_pl_ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            (void)kos_handle_close(g_pl_ep);
            tap::skip("task pool too small");
            return;
        }
        int const granted = kos_task_sched_grant(t, 0, 0x1);
        // TRANSFER as well as SIGNAL: the worker hands this endpoint on to the member it
        // seats into the nested task, and that member is what reports the inherited set.
        kos_cap_grant caps[] = {{g_pl_ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER}};
        auto m = kos::thread::create_caps(gi_worker, nullptr, "ginhw", 11, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr, t);
        bool const seated = m.valid();
        int32_t rep[GI_WORDS] = {0, 0, -1};
        bool heard = false;
        if (seated)
        {
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, g_pl_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
            heard = kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(rep)), &opts)
                    == static_cast<int32_t>(sizeof(rep));
            (void)m.join();
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(g_pl_ep);
        TAP_CHECK(granted == 0);
        TAP_CHECK(seated);
        TAP_CHECK(heard);
        tap::diag("nested task made %d, member seated %d, cores/err %d",
                  static_cast<int>(rep[GI_MADE]), static_cast<int>(rep[GI_SEATED]),
                  static_cast<int>(rep[GI_CORES]));
        TAP_CHECK(rep[GI_MADE] == 1);
        TAP_CHECK(rep[GI_SEATED] == 1);
        // 0x1 and not PL_ALL: a task that did not inherit reads as the whole machine.
        TAP_CHECK(rep[GI_CORES] == 0x1);
    }

    void t_grant_after_member_refused()
    {
        if (kos_sem_create(0, &g_pl_gate) != 0)
        {
            tap::skip("semaphore pool too small");
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            (void)kos_handle_close(g_pl_gate);
            tap::skip("task pool too small");
            return;
        }
        int const empty_ok = kos_task_sched_grant(t, 0, 0x3);
        kos_cap_grant caps[] = {{g_pl_gate, CH_FULL}};
        auto m = kos::thread::create_caps(pl_gate_worker, nullptr, "gbusy", 9, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          t);
        bool const seated = m.valid();
        int busy = -99;
        int mj = -1;
        if (seated)
        {
            busy = kos_task_sched_grant(t, 0, 0x1);
            kos_sem_post(g_pl_gate);
            mj = m.join();
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(g_pl_gate);
        TAP_CHECK(empty_ok == 0);
        TAP_CHECK(seated);
        TAP_CHECK(mj == 0);
        // Thread::affinity is a subset of the set and nothing re-derives it, so narrowing under
        // a live member would strand it.
        TAP_CHECK(busy == -KOS_EBUSY);
    }

    // A grant of exactly one isolated core and nothing else: the default set is then the grant,
    // so an unpinned member runs on the isolated core.
    void t_isolated_single_grant_ok()
    {
        uint32_t const iso = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_ISOLATED));
        if (iso == 0)
        {
            tap::skip("this image isolates no core");
            return;
        }
        uint32_t const core = pl_lowest(iso);
        uint32_t const bit = 1u << core;
        if (kos_endpoint_create(&g_pl_ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            (void)kos_handle_close(g_pl_ep);
            tap::skip("task pool too small");
            return;
        }
        int const granted = kos_task_sched_grant(t, 0, bit);
        kos_cap_grant caps[] = {{g_pl_ep, KOS_CAP_SIGNAL}};
        auto m = kos::thread::create_caps(gn_worker, nullptr, "isogr", 11, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          t);
        bool const seated = m.valid();
        int32_t rep[GN_WORDS] = {0, 0, 0};
        bool heard = false;
        int mj = -1;
        if (seated)
        {
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, g_pl_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
            heard = kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(rep)), &opts)
                    == static_cast<int32_t>(sizeof(rep));
            mj = m.join();
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(g_pl_ep);
        tap::diag("granted isolated core %u alone: task cores 0x%x, affinity 0x%x, ran on %d",
                  static_cast<unsigned>(core), static_cast<unsigned>(rep[GN_CORES]),
                  static_cast<unsigned>(rep[GN_AFF]), static_cast<int>(rep[GN_CORE]));
        TAP_CHECK(granted == 0);
        TAP_CHECK(seated);
        TAP_CHECK(heard);
        TAP_CHECK(mj == 0);
        TAP_CHECK(rep[GN_CORES] == static_cast<int32_t>(bit));
        TAP_CHECK(rep[GN_AFF] == static_cast<int32_t>(bit));
        TAP_CHECK(rep[GN_CORE] == static_cast<int32_t>(core));
    }

    void pl_exit_worker(void*)
    {
    }

    // An exited but unreclaimed slot still gen-matches, so the handle resolves to nothing left
    // to place; kill and slay give the same answer.
    void t_affinity_dead_handle_refused()
    {
        auto w = kos::thread::create(pl_exit_worker, nullptr, "adead", 9);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        int const joined = w.join();
        int const dead = kos_thread_set_affinity(w.id(), 0x1u);
        TAP_CHECK(joined == 0);
        TAP_CHECK(dead == -KOS_EBADF);
    }

    // A thread that names no core is given the task's set less the isolated cores.
    void t_isolated_unpinned_never()
    {
        uint32_t const iso = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_ISOLATED));
        if (iso == 0)
        {
            tap::skip("this image isolates no core");
            return;
        }
        pl_reset();
        auto w = kos::thread::create(pl_sample_worker, nullptr, "isonv", 12);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        g_pl_go = 1;
        int const joined = w.join();
        uint32_t const aff = g_pl_aff;
        uint32_t const seen = g_pl_seen;
        tap::diag("unpinned worker: affinity 0x%x, cores seen 0x%x, isolated 0x%x",
                  static_cast<unsigned>(aff), static_cast<unsigned>(seen),
                  static_cast<unsigned>(iso));
        TAP_CHECK(joined == 0);
        TAP_CHECK(aff == (PL_ALL & ~iso));
        TAP_CHECK(seen != 0); // the denominator: a worker that never ran would satisfy the rest
        TAP_CHECK((seen & iso) == 0);
    }

    // Unpin off an isolated core restores the DEFAULT set and not the grant: widened to the
    // grant, the thread would keep running on the isolated core it holds.
    void t_isolated_unpin_excludes()
    {
        uint32_t const iso = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_ISOLATED));
        if (iso == 0)
        {
            tap::skip("this image isolates no core");
            return;
        }
        uint32_t const core = pl_lowest(iso);
        pl_reset();
        auto w = kos::thread::create_caps(pl_sample_worker, nullptr, "isoup", 12, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, 1u << core);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        int const rc = kos::thread::unpin(w.id());
        g_pl_go = 1;
        int const joined = w.join();
        uint32_t const aff = g_pl_aff;
        uint32_t const seen = g_pl_seen;
        tap::diag("unpinned off isolated core %u: affinity 0x%x, cores seen 0x%x, isolated 0x%x",
                  static_cast<unsigned>(core), static_cast<unsigned>(aff),
                  static_cast<unsigned>(seen), static_cast<unsigned>(iso));
        TAP_CHECK(rc == 0);
        TAP_CHECK(joined == 0);
        TAP_CHECK(aff == (PL_ALL & ~iso));
        TAP_CHECK(seen != 0); // the denominator: a worker that never ran would satisfy the rest
        TAP_CHECK((seen & iso) == 0);
    }

    void t_isolated_takes_pinned()
    {
        uint32_t const iso = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_ISOLATED));
        if (iso == 0)
        {
            tap::skip("this image isolates no core");
            return;
        }
        uint32_t const core = pl_lowest(iso);
        pl_reset();
        auto w = kos::thread::create(pl_sample_worker, nullptr, "isopn", 12);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        int const rc = kos::thread::pin(w.id(), core);
        g_pl_go = 1;
        int const joined = w.join();
        uint32_t const aff = g_pl_aff;
        uint32_t const seen = g_pl_seen;
        tap::diag("pinned to isolated core %u: affinity 0x%x, cores seen 0x%x",
                  static_cast<unsigned>(core), static_cast<unsigned>(aff),
                  static_cast<unsigned>(seen));
        TAP_CHECK(rc == 0);
        TAP_CHECK(joined == 0);
        TAP_CHECK(aff == (1u << core));
        TAP_CHECK(seen == (1u << core));
    }

    // An isolated core named beside an ordinary one: the mask is admitted whole and verbatim.
    // Which of the two the thread is seen on is the picker's choice and is not asserted.
    void t_isolated_mixed_mask_ok()
    {
        uint32_t const iso = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_ISOLATED));
        if (iso == 0)
        {
            tap::skip("this image isolates no core");
            return;
        }
        uint32_t const mixed = iso | 1u; // core 0 can never be isolated, so the two are disjoint
        pl_reset();
        auto w = kos::thread::create(pl_sample_worker, nullptr, "isomx", 12);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        int const rc = kos_thread_set_affinity(w.id(), mixed);
        g_pl_go = 1;
        int const joined = w.join();
        uint32_t const aff = g_pl_aff;
        uint32_t const seen = g_pl_seen;
        tap::diag("isolated core named beside core 0: asked 0x%x, affinity 0x%x, cores seen 0x%x",
                  static_cast<unsigned>(mixed), static_cast<unsigned>(aff),
                  static_cast<unsigned>(seen));
        TAP_CHECK(rc == 0);
        TAP_CHECK(joined == 0);
        TAP_CHECK(aff == mixed);
        TAP_CHECK(seen != 0);
        TAP_CHECK((seen & ~mixed) == 0);
    }

    // --- Preemption: every core's own slice timer takes a thread off it -------------------
    // KOS_SCHED_OP_PREEMPTED is machine-wide, so it can witness a SECONDARY arming its own
    // comparator.
    //
    // The probe records an expiry only where the tick changed the running thread
    // (kernel/time/time.cc compares sched::current() around sched::tick_rr). An expiry on a
    // core carrying ONE runnable thread of its priority rotates the incumbent to itself and
    // records nothing, however long it runs. So each ROUND pins an equal-priority pair to the
    // core under test and one burner to each other core: an unpinned crowd can settle one per
    // core, and a core that never carried a pair reads exactly like a comparator that never
    // fired.
    //
    // KICKOS_KERNEL_CORES + 1: tests/integration/gates/selftest.cmake derives the expected
    // pool-too-small skip from this count.
    constexpr unsigned PL_CROWD = KICKOS_KERNEL_CORES + 1u;
    constexpr uint64_t PL_BURN_QUANTA = 8ull; // per round, so a slice can expire several times

    // Read-only to the burners, published before the first one is created.
    uint64_t g_pl_burn_ns = 32000000ull;

    // `first`/`last` are the low 32 bits of the clock at a burner's first and last sample, both
    // instants it was seen executing, so their span is not wall clock the host can inflate. A
    // span of one quantum contains the deadline armed at switch-in, so the closing sample
    // cannot have run unless the core took that deadline's interrupt.
    Atomic<uint32_t, Order::RELAXED> g_pl_burn_seen[PL_CROWD];
    Atomic<uint32_t, Order::RELAXED> g_pl_burn_first[PL_CROWD];
    Atomic<uint32_t, Order::RELAXED> g_pl_burn_last[PL_CROWD];
    Atomic<uint32_t, Order::RELAXED> g_pl_burn_samples[PL_CROWD];

    // Spins, never yields or sleeps: the slice has to EXPIRE under this thread for the timer
    // to be what takes the core away. The core probe does not yield either.
    void pl_burn_worker(void* arg)
    {
        unsigned const me = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
        // Released together, so both of a pair are queued before either arms a slice. A burner
        // runs the instant it is created, and on a slow host the first could burn its whole
        // round before root creates the second, which reads as a comparator that never fired.
        pl_wait_go();
        uint64_t const start = kos_clock_now();
        uint32_t seen = 0;
        uint32_t samples = 0;
        uint32_t first = 0;
        uint32_t last = 0;
        uint32_t pass = 0;
        while (true)
        {
            uint64_t const now = kos_clock_now();
            if (now - start >= g_pl_burn_ns)
            {
                break;
            }
            pass++;
            if ((pass & 0xFu) == 0u)
            {
                seen |= 1u << static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_CORE));
                last = static_cast<uint32_t>(now);
                if (samples == 0u)
                {
                    first = last;
                }
                samples++;
            }
        }
        g_pl_burn_first[me] = first;
        g_pl_burn_last[me] = last;
        g_pl_burn_samples[me] = samples;
        g_pl_burn_seen[me] = seen;
    }

    // The n-th core of `mask` other than `skip`, wrapping, or `skip` when the mask names none.
    uint32_t pl_other_core(uint32_t mask, uint32_t skip, unsigned n)
    {
        uint32_t others[KICKOS_KERNEL_CORES];
        unsigned count = 0;
        for (uint32_t c = 0; c < static_cast<uint32_t>(KICKOS_KERNEL_CORES); c++)
        {
            if (c != skip and (mask & (1u << c)) != 0u)
            {
                others[count] = c;
                count++;
            }
        }
        if (count == 0u)
        {
            return skip;
        }
        return others[n % count];
    }

    void t_slice_preempts_every_core()
    {
        uint32_t const iso = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_ISOLATED));
        // Isolated cores are outside the claim. Core 0 can never be isolated, so the target is
        // never empty.
        uint32_t const want = PL_ALL & ~iso;
        // Before the baseline read, so the pool probe's own threads contribute no bit to
        // `after`.
        if (not pool_can_host(static_cast<int>(PL_CROWD)))
        {
            tap::skip("pool too small (%u concurrent burners)", PL_CROWD);
            return;
        }
        uint32_t const before = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_PREEMPTED));

        // The quantum must be resolvable by the monotonic clock or no slice can expire under a
        // burn, and an emulated clock's granule is coarse, so the granule is measured.
        uint64_t e0 = kos_clock_now();
        uint64_t e1 = e0;
        while (e1 == e0)
        {
            e1 = kos_clock_now();
        }
        uint64_t e2 = e1;
        while (e2 == e1)
        {
            e2 = kos_clock_now();
        }
        uint64_t const granule = e2 - e1;
        uint64_t quantum = 1000000ull; // 1 ms on a fine clock
        if (quantum < granule * 4)
        {
            quantum = granule * 4;
        }
        g_pl_burn_ns = quantum * PL_BURN_QUANTA;

        uint32_t prewitnessed = 0; // the probe is monotonic, so these owe this arm no round
        uint32_t unproven = 0;     // ran their round and the bit stayed clear
        uint32_t starved = 0;      // ... and the pinned pair never held the core a whole quantum
        uint32_t strayed = 0;      // ... and the pair did not stay on the core it was given
        uint32_t after = before;

        for (uint32_t k = 0; k < static_cast<uint32_t>(KICKOS_KERNEL_CORES); k++)
        {
            if ((want & (1u << k)) == 0u)
            {
                continue;
            }
            after = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_PREEMPTED));
            if ((after & (1u << k)) != 0u)
            {
                // Already recorded, and the mask never clears. Only that core's own comparator
                // can have set the bit, which is the whole claim.
                prewitnessed |= 1u << k;
                continue;
            }
            for (unsigned i = 0; i < PL_CROWD; i++)
            {
                g_pl_burn_seen[i] = 0;
                g_pl_burn_first[i] = 0;
                g_pl_burn_last[i] = 0;
                g_pl_burn_samples[i] = 0;
            }
            g_pl_go = 0; // released once every burner of the round exists
            kos::thread::Handle w[PL_CROWD];
            unsigned made = 0;
            for (unsigned i = 0; i < PL_CROWD; i++)
            {
                // Burners 0 and 1 are the pair on the core under test. The rest load the other
                // cores and nothing is asserted of them.
                uint32_t pin = 1u << k;
                if (i >= 2u)
                {
                    pin = 1u << pl_other_core(want, k, i - 2u);
                }
                // The mask goes in at CREATE: pinning afterwards leaves a window in which the
                // burner is runnable anywhere, and it only takes one sample there to make the
                // pair look strayed.
                w[i] = kos::thread::create_caps(pl_burn_worker,
                                                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                                                "burn", 12, nullptr, 0, KOS_POLICY_RR,
                                                static_cast<uint32_t>(quantum), false, nullptr, 0,
                                                0, nullptr, KOS_TASK_NONE, nullptr, 0, pin);
                if (not w[i].valid())
                {
                    break;
                }
                made++;
            }
            g_pl_go = 1;
            int joined = 0;
            for (unsigned i = 0; i < made; i++)
            {
                joined |= w[i].join();
            }
            // The probe at the top just held PL_CROWD slots and stacks, so a short crowd here
            // is a pool bug and not a small board.
            TAP_CHECK(made == PL_CROWD);
            TAP_CHECK(joined == 0);

            after = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_PREEMPTED));
            if ((after & (1u << k)) != 0u)
            {
                continue;
            }
            unproven |= 1u << k;
            uint32_t span = 0;
            uint32_t pair_samples = 0;
            uint32_t pair_seen = 0;
            for (unsigned i = 0; i < 2u; i++)
            {
                pair_seen |= g_pl_burn_seen[i].load();
                pair_samples += g_pl_burn_samples[i].load();
                if (g_pl_burn_samples[i].load() >= 2u)
                {
                    uint32_t const s = g_pl_burn_last[i].load() - g_pl_burn_first[i].load();
                    if (s > span)
                    {
                        span = s;
                    }
                }
            }
            if ((pair_seen & ~(1u << k)) != 0u)
            {
                strayed |= 1u << k;
            }
            else if (span < static_cast<uint32_t>(quantum))
            {
                starved |= 1u << k;
            }
            tap::diag("core %u: no preemption recorded; its pinned pair held it for %u us "
                      "across %u sample(s), against a %u us quantum",
                      static_cast<unsigned>(k), static_cast<unsigned>(span / 1000u),
                      static_cast<unsigned>(pair_samples),
                      static_cast<unsigned>(quantum / 1000u));
        }

        tap::diag("quantum %u ns, %u round(s) of %llu quanta with a pinned pair: slice "
                  "preemptions 0x%x -> 0x%x, wanted 0x%x, already witnessed 0x%x, unproven 0x%x,"
                  " starved 0x%x",
                  static_cast<unsigned>(quantum), static_cast<unsigned>(KICKOS_KERNEL_CORES),
                  static_cast<unsigned long long>(PL_BURN_QUANTA), static_cast<unsigned>(before),
                  static_cast<unsigned>(after), static_cast<unsigned>(want),
                  static_cast<unsigned>(prewitnessed), static_cast<unsigned>(unproven),
                  static_cast<unsigned>(starved));
        // Monotonic, so a bit that went away is a torn read of a cell with one writer.
        TAP_CHECK((before & ~after) == 0u);
        // A pair seen off its core means the round staged something else.
        TAP_CHECK(strayed == 0u);
        // Asserted only where the pair held its core a whole quantum: without one there was no
        // expiry to take it off, and the host took the window, not an unarmed comparator.
        uint32_t const denied = unproven & ~starved;
        TAP_CHECK(denied == 0u);
        if (unproven != 0u)
        {
            TAP_SKIP_VACUOUS("no preemption on core(s) 0x%x, and their pinned pair never held "
                             "the core a whole %u us quantum: the window went, not the "
                             "comparator",
                             static_cast<unsigned>(starved),
                             static_cast<unsigned>(quantum / 1000u));
            return;
        }
    }

    // --- Placement: a thread of this app is seen running on every core --------------------
    // The union across an unpinned crowd has to NAME every core the crowd may run on, which no
    // single-core placement satisfies.
    //
    // One cell per worker: a shared mask is a cross-core read-modify-write, and a lost update
    // erases a core from the very union being read.
    Atomic<uint32_t, Order::RELAXED> g_pl_spread[PL_CROWD];
    Atomic<uint32_t, Order::RELAXED> g_pl_spread_aff[PL_CROWD];
    // Samples taken, to tell a short union from a starved one: the budget is wall clock, which a
    // loaded host can spend without offering the crowd its rotations.
    Atomic<uint32_t, Order::RELAXED> g_pl_spread_passes[PL_CROWD];
    uint32_t g_pl_spread_want = 0; // published before the crowd is released

    // A time budget, sampled until the union is complete: root holds core 0 until it blocks in
    // the join below, so a crowd spending a fixed sample count can finish before core 0 is free
    // and read as never having reached it.
    constexpr uint64_t PL_SPREAD_BUDGET_NS = 200000000ull; // 200 ms

    void pl_spread_worker(void* arg)
    {
        unsigned const me = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
        pl_wait_go();
        g_pl_spread_aff[me] = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_AFFINITY));
        uint64_t const start = kos_clock_now();
        uint32_t seen = 0;
        uint32_t passes = 0;
        while (true)
        {
            seen |= 1u << static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_CORE));
            passes++;
            g_pl_spread_passes[me] = passes;
            // Published every pass: the stop condition is the union across the crowd.
            g_pl_spread[me] = seen;
            uint32_t all = 0;
            for (unsigned j = 0; j < PL_CROWD; j++)
            {
                all |= g_pl_spread[j].load();
            }
            if ((all & g_pl_spread_want) == g_pl_spread_want)
            {
                break;
            }
            if (kos_clock_now() - start >= PL_SPREAD_BUDGET_NS)
            {
                break;
            }
            // Yield, never sleep: a sleeping worker is not part of the crowd that makes a core
            // give one of them up.
            kos_yield();
        }
        g_pl_spread[me] = seen;
    }

    void t_threads_reach_every_core()
    {
        uint32_t const iso = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_ISOLATED));
        // An isolated core is held out of the default core set and this crowd names no core.
        // Core 0 can never be isolated, so the target is never empty.
        uint32_t const want = PL_ALL & ~iso;

        pl_reset();
        g_pl_spread_want = want;
        for (unsigned i = 0; i < PL_CROWD; i++)
        {
            g_pl_spread[i] = 0;
            g_pl_spread_aff[i] = 0;
            g_pl_spread_passes[i] = 0;
        }
        // Before the first spawn: see the shortfall check below.
        if (not pool_can_host(static_cast<int>(PL_CROWD)))
        {
            tap::skip("pool too small (%u concurrent workers)", PL_CROWD);
            return;
        }
        kos::thread::Handle w[PL_CROWD];
        unsigned made = 0;
        for (unsigned i = 0; i < PL_CROWD; i++)
        {
            w[i] = kos::thread::create(pl_spread_worker,
                                       reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                                       "spread", 12);
            if (not w[i].valid())
            {
                break;
            }
            made++;
        }
        // The whole crowd or nothing: below it, a core carrying no worker is a core the run had
        // none to give it.
        if (made < PL_CROWD)
        {
            for (unsigned i = 0; i < made; i++)
            {
                (void)w[i].kill();
                (void)w[i].join();
            }
            // The probe above just held PL_CROWD slots and stacks, so a short crowd here is a
            // pool bug and not a small board.
            TAP_CHECK(made == PL_CROWD);
            return;
        }
        // Last, once every worker exists: released one at a time, the first could finish its
        // samples before anyone could crowd it off a core.
        g_pl_go = 1;
        int joined = 0;
        for (unsigned i = 0; i < PL_CROWD; i++)
        {
            joined |= w[i].join();
        }

        uint32_t reached = 0;
        uint32_t unpinned = 0;
        uint32_t fewest = 0xFFFFFFFFu;
        for (unsigned i = 0; i < PL_CROWD; i++)
        {
            reached |= g_pl_spread[i].load();
            if (g_pl_spread_aff[i].load() == want)
            {
                unpinned++;
            }
            uint32_t const passes = g_pl_spread_passes[i].load();
            if (passes < fewest)
            {
                fewest = passes;
            }
        }
        // A pass is one sample and one yield, so a worker needs a pass per target core to be
        // SEEN on all of them. Four per core leaves the placement three chances beyond the
        // minimum before a short union counts against it.
        uint32_t targets = 0;
        for (unsigned c = 0; c < KICKOS_KERNEL_CORES; c++)
        {
            if ((want & (1u << c)) != 0u)
            {
                targets++;
            }
        }
        uint32_t const floor_passes = 4u * targets;
        tap::diag("%u unpinned worker(s) sampling to completion: cores reached 0x%x, wanted 0x%x,"
                  " fewest passes %u (floor %u)",
                  PL_CROWD, static_cast<unsigned>(reached), static_cast<unsigned>(want),
                  static_cast<unsigned>(fewest), static_cast<unsigned>(floor_passes));
        TAP_CHECK(joined == 0);
        // A crowd the kernel had pinned would satisfy the union below with no choice made.
        TAP_CHECK(unpinned == PL_CROWD);
        // A short union is a placement failure only if the crowd was given its rotations;
        // otherwise the host spent the wall-clock budget, and the arm declines.
        if ((reached & want) != want and fewest < floor_passes)
        {
            TAP_SKIP_VACUOUS("the %u ms budget left the slowest worker at %u of %u sample(s): "
                             "0x%x of 0x%x reached for want of rotations, not placement",
                             static_cast<unsigned>(PL_SPREAD_BUDGET_NS / 1000000ull),
                             static_cast<unsigned>(fewest), static_cast<unsigned>(floor_passes),
                             static_cast<unsigned>(reached), static_cast<unsigned>(want));
            return;
        }
        TAP_CHECK((reached & want) == want);
    }

    // A fused reply followed by a park must notify the caller's core. A FIFO spinner occupies
    // that core, so only a reschedule request lets the higher-priority caller run.
    constexpr uint8_t XC_SPIN_PRIO = 14;   // above root, below the caller
    constexpr uint8_t XC_CALLER_PRIO = 20; // the thread the reply readies
    constexpr uint8_t XC_SERVER_PRIO = 12;
    constexpr uint32_t XC_JOIN_US = 400000;
    kos_cap_t g_xc_ep = KOS_CAP_NONE;
    kos_cap_t g_xc_gate = KOS_CAP_NONE;
    Atomic<uint32_t, Order::RELAXED> g_xc_stop{0};
    Atomic<uint32_t, Order::RELAXED> g_xc_spin_core{0xFFFFFFFFu};
    Atomic<int32_t, Order::RELAXED> g_xc_call_rc{-99};
    Atomic<int32_t, Order::RELAXED> g_xc_serve_rc{-99};

    void xc_server(void*) // caps: E(WAIT)@1, gate@2
    {
        char buf[16];
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 1, 0, KOS_TIMEOUT_NONE);
        int32_t const got =
            kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        if (got < 0 or opts.info.reply_cap == KOS_CAP_NONE)
        {
            g_xc_serve_rc = -1;
            return;
        }
        // Reply only once the caller is parked for it and the spinner holds the caller's core.
        kos_sem_wait(2);
        kos_cap_t const reply = opts.info.reply_cap;
        kos_reply_recv_opts_init(&opts, 1, 0, KOS_TIMEOUT_NONE);
        memcpy(buf, "pong!", 5);
        g_xc_serve_rc = kos_reply_recv(reply, buf, kos_call_lens_pack(5, sizeof(buf)), &opts);
    }
    void xc_caller(void*) // caps: E(SIGNAL)@1
    {
        char buf[16];
        memcpy(buf, "ping", 4);
        g_xc_call_rc = kos_call(1, buf, 4, sizeof(buf));
    }
    // The spinner yields its core only to a preemption and the reply follows its post, so the
    // core it posts from is held when the reply lands. A spin count cannot witness that: a
    // reschedule arriving before the first iteration leaves it at zero.
    void xc_spinner(void*) // caps: gate@1
    {
        g_xc_spin_core = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_CORE));
        kos_sem_post(1); // the core is ours; the server may answer now
        while (g_xc_stop.load() == 0)
        {
        }
    }
    void t_resched_reaches_pinned_caller()
    {
        uint32_t const iso = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_ISOLATED));
        uint32_t away = 0;
        unsigned found = 0;
        for (uint32_t c = 1; c < static_cast<uint32_t>(KICKOS_KERNEL_CORES); c++)
        {
            if ((iso & (1u << c)) != 0)
            {
                continue;
            }
            away = c;
            found = 1;
            break;
        }
        if (found == 0)
        {
            tap::skip("a cross-core wake needs a non-isolated core beside the boot core");
            return;
        }
        g_xc_stop = 0;
        g_xc_spin_core = 0xFFFFFFFFu;
        g_xc_call_rc = -99;
        g_xc_serve_rc = -99;
        if (kos_endpoint_create(&g_xc_ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        if (kos_sem_create(0, &g_xc_gate) != 0)
        {
            (void)kos_handle_close(g_xc_ep);
            tap::skip("semaphore pool too small");
            return;
        }
        kos_cap_grant vcaps[] = {{g_xc_ep, KOS_CAP_WAIT}, {g_xc_gate, CH_FULL}};
        kos_cap_grant ccaps[] = {{g_xc_ep, KOS_CAP_SIGNAL}};
        kos_cap_grant pcaps[] = {{g_xc_gate, CH_FULL}};
        auto sv = kos::thread::create_caps(xc_server, nullptr, "xcS", XC_SERVER_PRIO, vcaps, 2,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                           KOS_TASK_NONE, nullptr, 0, 1u << PL_HOME);
        kos::thread::Handle cl;
        kos::thread::Handle sp;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // let the server park in its receive
            cl = kos::thread::create_caps(xc_caller, nullptr, "xcC", XC_CALLER_PRIO, ccaps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, 1u << away);
        }
        if (cl.valid())
        {
            kos_sleep_ns(3000000ull); // let the caller park awaiting its reply
            sp = kos::thread::create_caps(xc_spinner, nullptr, "xcP", XC_SPIN_PRIO, pcaps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, 1u << away);
        }
        if (not sv.valid() or not cl.valid() or not sp.valid())
        {
            g_xc_stop = 1;
            if (sp.valid())
            {
                (void)sp.join();
            }
            (void)kos_handle_close(g_xc_ep);
            kos_sem_destroy(g_xc_gate);
            tap::skip("pool too small for 3 threads");
            return;
        }
        // Bounded so a missing reschedule request fails instead of hanging.
        int const cjoined = cl.join(XC_JOIN_US);
        g_xc_stop = 1;
        int const sjoined = sp.join();
        // Release the server from its final receive.
        (void)kos_send(g_xc_ep, "", 0);
        int const vjoined = sv.join();
        (void)kos_handle_close(g_xc_ep);
        kos_sem_destroy(g_xc_gate);
        uint32_t const spin_core = g_xc_spin_core;
        tap::diag("caller pinned to core %u under a spinner on core %u: join %d, call %d",
                  static_cast<unsigned>(away), static_cast<unsigned>(spin_core), cjoined,
                  static_cast<int>(g_xc_call_rc.load()));
        TAP_CHECK(sjoined == 0);
        TAP_CHECK(vjoined == 0);
        TAP_CHECK(spin_core == away);
        TAP_CHECK(cjoined == 0);
        TAP_CHECK(g_xc_call_rc.load() == 5);
    }

    // --- IRQ delivery across cores ----------------------------------------------------------
    // Every raise of a claimed line is taken on the core that claimed it, whichever core
    // raises it, and a raise one core still holds for a released line reaches no later owner.
    constexpr int XIRQ_LINE = KICKOS_IRQ_FREE_BASE + 5;
    constexpr int XIRQ_STALE_LINE = KICKOS_IRQ_FREE_BASE + 8;
    constexpr uint32_t XIRQ_CLAIM_CORE = 1;
    constexpr uint32_t XIRQ_OTHER_CORE = 0;
    constexpr uint32_t XIRQ_ALL = 0xFFFFFFFFu;
    constexpr uint32_t XIRQ_WAKE_US = 200000u;
    constexpr uint32_t XIRQ_QUIET_US = 20000u;
    constexpr uint64_t XIRQ_CLAIM_BUDGET_NS = 2000000000ull;
    constexpr uint64_t XIRQ_CLAIM_POLL_NS = 100000ull;
    constexpr uint8_t XIRQ_PRIO = 12;
    constexpr int XIRQ_REL = 3; // root-to-child release, delegated after done and ready
    constexpr int32_t XIRQ_UNSET = -99;

    // A released line is claimable again only once its retirement's grace period has passed.
    int xirq_claim(int line, kos_cap_t* out)
    {
        uint64_t const deadline = kos_clock_now() + XIRQ_CLAIM_BUDGET_NS;
        int rc = kos_irq_claim(line, KOS_IRQ_EDGE, out);
        while (rc == -KOS_EBUSY and kos_clock_now() <= deadline)
        {
            kos_sleep_ns(XIRQ_CLAIM_POLL_NS);
            rc = kos_irq_claim(line, KOS_IRQ_EDGE, out);
        }
        return rc;
    }

    // Claims `line` and makes the caller the waiter of a fresh notification it signals. 0, or
    // the first refusal.
    int xirq_own(int line, kos_cap_t* irq, kos_cap_t* note)
    {
        int rc = xirq_claim(line, irq);
        if (rc != 0)
        {
            return rc;
        }
        rc = kos_notify_create(note);
        if (rc != 0)
        {
            return rc;
        }
        rc = kos_irq_bind_notify(*irq, *note);
        if (rc != 0)
        {
            return rc;
        }
        return kos_notify_bind(*note);
    }

    kos::thread::Handle xirq_spawn(void (*entry)(void*), void* arg, char const* name,
                                   kos_cap_grant const* caps, uint8_t count, uint32_t core)
    {
        return kos::thread::create_caps(entry, arg, name, XIRQ_PRIO, caps, count,
                                        KOS_POLICY_FIFO, 0, false, nullptr, 0, KOS_AUTH_IRQ,
                                        nullptr, KOS_TASK_NONE, nullptr, 0, 1u << core);
    }

    uint32_t xirq_core()
    {
        return static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_CORE));
    }

    Atomic<int32_t, Order::RELAXED> g_xw_own{XIRQ_UNSET};
    Atomic<int32_t, Order::RELAXED> g_xw_arm{XIRQ_UNSET};
    Atomic<int32_t, Order::RELAXED> g_xw_first{XIRQ_UNSET};
    Atomic<int32_t, Order::RELAXED> g_xw_second{XIRQ_UNSET};
    Atomic<uint32_t, Order::RELAXED> g_xw_bits{0};
    Atomic<uint32_t, Order::RELAXED> g_xw_core{0xffu};
    Atomic<int32_t, Order::RELAXED> g_xw_inject{XIRQ_UNSET};
    Atomic<uint32_t, Order::RELAXED> g_xw_inject_core{0xffu};

    // caps: done@1, ready@2. Armed before it reports ready, so the raise root orders next
    // lands on an armed line and not in the latch the first arm discards.
    void xirq_waiter(void*)
    {
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        g_xw_own = xirq_own(XIRQ_LINE, &irq, &note);
        if (g_xw_own.load() == 0)
        {
            g_xw_arm = kos_irq_ack(irq);
        }
        kos_sem_post(CH_READY);
        if (g_xw_own.load() == 0 and g_xw_arm.load() == 0)
        {
            uint32_t bits = 0;
            g_xw_first = kos_notify_wait(note, XIRQ_ALL, XIRQ_WAKE_US, &bits);
            g_xw_core = xirq_core();
            g_xw_bits = bits;
            bits = 0;
            g_xw_second = kos_notify_wait(note, XIRQ_ALL, XIRQ_QUIET_US, &bits);
        }
        kos_handle_close(irq);
        kos_handle_close(note);
        kos_sem_post(CH_DONE);
    }

    // caps: done@1, go@2.
    void xirq_injector(void*)
    {
        kos_sem_wait(CH_READY);
        g_xw_inject_core = xirq_core();
        g_xw_inject = kos_irq_inject(XIRQ_LINE);
        kos_sem_post(CH_DONE);
    }

    void t_irq_cross_core_wake()
    {
        g_xw_own = XIRQ_UNSET;
        g_xw_arm = XIRQ_UNSET;
        g_xw_first = XIRQ_UNSET;
        g_xw_second = XIRQ_UNSET;
        g_xw_bits = 0;
        g_xw_core = 0xffu;
        g_xw_inject = XIRQ_UNSET;
        g_xw_inject_core = 0xffu;
        kos_cap_t ready = KOS_CAP_NONE;
        kos_cap_t go = KOS_CAP_NONE;
        if (kos_sem_create(0, &ready) != 0 or kos_sem_create(0, &go) != 0)
        {
            kos_sem_destroy(ready);
            tap::skip("semaphore pool too small");
            return;
        }
        kos_cap_grant wcaps[] = {{g_done, CH_FULL}, {ready, CH_FULL}};
        kos_cap_grant icaps[] = {{g_done, CH_FULL}, {go, CH_FULL}};
        auto w = xirq_spawn(xirq_waiter, nullptr, "xirqW", wcaps, 2, XIRQ_CLAIM_CORE);
        if (not w.valid())
        {
            kos_sem_destroy(ready);
            kos_sem_destroy(go);
            tap::skip("thread pool too small");
            return;
        }
        kos_sem_wait(ready);
        auto inj = xirq_spawn(xirq_injector, nullptr, "xirqI", icaps, 2, XIRQ_OTHER_CORE);
        int ijoined = 0;
        if (inj.valid())
        {
            kos_sem_post(go);
            wait_n(1);
            ijoined = inj.join();
        }
        wait_n(1);
        int const wjoined = w.join();
        kos_sem_destroy(ready);
        kos_sem_destroy(go);
        if (not inj.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        tap::diag("claimed on core %u, injected from core %u: first wait %d bits 0x%x on "
                  "core %u, second wait %d",
                  static_cast<unsigned>(XIRQ_CLAIM_CORE),
                  static_cast<unsigned>(g_xw_inject_core.load()),
                  static_cast<int>(g_xw_first.load()), static_cast<unsigned>(g_xw_bits.load()),
                  static_cast<unsigned>(g_xw_core.load()),
                  static_cast<int>(g_xw_second.load()));
        TAP_CHECK(wjoined == 0);
        TAP_CHECK(ijoined == 0);
        TAP_CHECK(g_xw_own.load() == 0);
        TAP_CHECK(g_xw_arm.load() == 0);
        TAP_CHECK(g_xw_inject.load() == 0);
        TAP_CHECK(g_xw_inject_core.load() == XIRQ_OTHER_CORE);
        TAP_CHECK(g_xw_first.load() == 0);
        TAP_CHECK(g_xw_bits.load() == 1u);
        TAP_CHECK(g_xw_core.load() == XIRQ_CLAIM_CORE);
        // Exactly once: one raise, one wake.
        TAP_CHECK(g_xw_second.load() == -KOS_ETIMEDOUT);
    }

    enum
    {
        XS_OLD = 0,   // the first owner, which leaves a raise latched and releases
        XS_OTHER = 1, // the next owner, on another core
        XS_BACK = 2,  // the one after, back on the first owner's core
        XS_LEGS = 3
    };
    Atomic<int32_t, Order::RELAXED> g_xs_own[XS_LEGS];
    Atomic<int32_t, Order::RELAXED> g_xs_first[XS_LEGS];
    Atomic<int32_t, Order::RELAXED> g_xs_second[XS_LEGS];
    Atomic<uint32_t, Order::RELAXED> g_xs_core[XS_LEGS];

    // caps: done@1, ready@2, release@3. Claims and attaches the line and never arms it, so the
    // raise root lands while it is held stays latched until the release.
    void xirq_stale_owner(void*)
    {
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        g_xs_own[XS_OLD] = xirq_own(XIRQ_STALE_LINE, &irq, &note);
        kos_sem_post(CH_READY);
        kos_sem_wait(XIRQ_REL);
        kos_handle_close(irq);
        kos_handle_close(note);
        kos_sem_post(CH_DONE);
    }

    // caps: done@1, ready@2. `arg` is the leg. Its first wait arms the line, and must find no
    // raise: the only one so far was landed for an earlier owner.
    void xirq_next_owner(void* arg)
    {
        int const leg = static_cast<int>(reinterpret_cast<intptr_t>(arg));
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        g_xs_own[leg] = xirq_own(XIRQ_STALE_LINE, &irq, &note);
        if (g_xs_own[leg].load() == 0)
        {
            uint32_t bits = 0;
            g_xs_first[leg] = kos_notify_wait(note, XIRQ_ALL, XIRQ_QUIET_US, &bits);
        }
        kos_sem_post(CH_READY);
        if (g_xs_own[leg].load() == 0)
        {
            uint32_t bits = 0;
            g_xs_second[leg] = kos_notify_wait(note, XIRQ_ALL, XIRQ_WAKE_US, &bits);
            g_xs_core[leg] = xirq_core();
        }
        kos_handle_close(irq);
        kos_handle_close(note);
        kos_sem_post(CH_DONE);
    }

    // One later owner on `core`: its first wait must stay quiet, and the raise root lands after
    // it must wake it there. False when the thread could not be made.
    bool xirq_next_leg(int leg, uint32_t core, kos_cap_t ready)
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {ready, CH_FULL}};
        auto t = xirq_spawn(xirq_next_owner, reinterpret_cast<void*>(static_cast<intptr_t>(leg)),
                            "xirqN", caps, 2, core);
        if (not t.valid())
        {
            return false;
        }
        kos_sem_wait(ready);
        (void)kos_irq_inject(XIRQ_STALE_LINE);
        wait_n(1);
        (void)t.join();
        return true;
    }

    void t_irq_reclaim_stale_raise()
    {
        for (int leg = 0; leg < XS_LEGS; leg++)
        {
            g_xs_own[leg] = XIRQ_UNSET;
            g_xs_first[leg] = XIRQ_UNSET;
            g_xs_second[leg] = XIRQ_UNSET;
            g_xs_core[leg] = 0xffu;
        }
        kos_cap_t ready = KOS_CAP_NONE;
        kos_cap_t rel = KOS_CAP_NONE;
        if (kos_sem_create(0, &ready) != 0 or kos_sem_create(0, &rel) != 0)
        {
            kos_sem_destroy(ready);
            tap::skip("semaphore pool too small");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {ready, CH_FULL}, {rel, CH_FULL}};
        auto old = xirq_spawn(xirq_stale_owner, nullptr, "xirqO", caps, 3, XIRQ_CLAIM_CORE);
        if (not old.valid())
        {
            kos_sem_destroy(ready);
            kos_sem_destroy(rel);
            tap::skip("thread pool too small");
            return;
        }
        kos_sem_wait(ready);
        int const irc = kos_irq_inject(XIRQ_STALE_LINE);
        kos_sem_post(rel);
        wait_n(1);
        int const ojoined = old.join();
        bool made = xirq_next_leg(XS_OTHER, XIRQ_OTHER_CORE, ready);
        if (made)
        {
            made = xirq_next_leg(XS_BACK, XIRQ_CLAIM_CORE, ready);
        }
        kos_sem_destroy(ready);
        kos_sem_destroy(rel);
        if (not made)
        {
            tap::skip("thread pool too small");
            return;
        }
        tap::diag("raise latched on core %u then released: core %u first wait %d, next raise "
                  "%d on core %u; core %u first wait %d, next raise %d on core %u",
                  static_cast<unsigned>(XIRQ_CLAIM_CORE), static_cast<unsigned>(XIRQ_OTHER_CORE),
                  static_cast<int>(g_xs_first[XS_OTHER].load()),
                  static_cast<int>(g_xs_second[XS_OTHER].load()),
                  static_cast<unsigned>(g_xs_core[XS_OTHER].load()),
                  static_cast<unsigned>(XIRQ_CLAIM_CORE),
                  static_cast<int>(g_xs_first[XS_BACK].load()),
                  static_cast<int>(g_xs_second[XS_BACK].load()),
                  static_cast<unsigned>(g_xs_core[XS_BACK].load()));
        TAP_CHECK(ojoined == 0);
        TAP_CHECK(irc == 0);
        for (int leg = 0; leg < XS_LEGS; leg++)
        {
            TAP_CHECK(g_xs_own[leg].load() == 0);
        }
        TAP_CHECK(g_xs_first[XS_OTHER].load() == -KOS_ETIMEDOUT);
        TAP_CHECK(g_xs_second[XS_OTHER].load() == 0);
        TAP_CHECK(g_xs_core[XS_OTHER].load() == XIRQ_OTHER_CORE);
        TAP_CHECK(g_xs_first[XS_BACK].load() == -KOS_ETIMEDOUT);
        TAP_CHECK(g_xs_second[XS_BACK].load() == 0);
        TAP_CHECK(g_xs_core[XS_BACK].load() == XIRQ_CLAIM_CORE);
    }

    // --- libc reentrant state across cores: two threads of one task at the same instant ----
#if KICKOS_LIBC_REENT
    // The checker holds EINVAL on core 1 while two switchers ping-pong on core 0, each switch
    // there being a seat for the incoming thread. Every errno is set by libc itself.
    constexpr unsigned RE_SWITCHERS = 2;
    constexpr uint32_t RE_ROUNDS = 2000;
    constexpr uint64_t RE_BUDGET_NS = 2000000000ull; // 2 s
    constexpr uint32_t RE_CHECK_CORE = 1;
    constexpr uint32_t RE_SWITCH_CORE = 0;
    Atomic<uint32_t, Order::RELAXED> g_re_go{0};
    Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> g_re_armed{0};
    Atomic<uint32_t, Order::RELAXED> g_re_rounds[RE_SWITCHERS];
    Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> g_re_done[RE_SWITCHERS];
    Atomic<uint32_t, Order::RELAXED> g_re_bad[RE_SWITCHERS + 1];
    Atomic<uint32_t, Order::RELAXED> g_re_core[RE_SWITCHERS + 1];
    Atomic<uint32_t, Order::RELAXED> g_re_reads{0};
    Atomic<uint32_t, Order::RELAXED> g_re_seen{0};
    Atomic<uint32_t, Order::RELAXED> g_re_finished{0};

    // strtol, not an assignment: the value must be written through libc's own state lookup.
    int re_provoke_einval()
    {
        char* end = nullptr;
        (void)strtol("10", &end, 99);
        return errno;
    }

    int re_provoke_erange()
    {
        char* end = nullptr;
        (void)strtol("99999999999999999999999999", &end, 10);
        return errno;
    }

    uint32_t re_rounds_total()
    {
        uint32_t total = 0;
        for (unsigned i = 0; i < RE_SWITCHERS; i++)
        {
            total += g_re_rounds[i].load();
        }
        return total;
    }

    bool re_switchers_done()
    {
        for (unsigned i = 0; i < RE_SWITCHERS; i++)
        {
            if (g_re_done[i].load() == 0)
            {
                return false;
            }
        }
        return true;
    }

    void re_wait_go()
    {
        while (g_re_go.load() == 0)
        {
            kos_sleep_ns(PLACE_STEP_NS);
        }
    }

    // Never blocks or yields inside the loop: a switch on this core would reseat its own state
    // and mask the peer core's.
    void re_checker(void*)
    {
        re_wait_go();
        g_re_core[0] = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_CORE));
        uint32_t bad = 0;
        if (re_provoke_einval() != EINVAL)
        {
            bad++;
        }
        uint32_t const before = re_rounds_total();
        g_re_armed = 1;
        uint64_t const start = kos_clock_now();
        uint32_t reads = 0;
        while (not re_switchers_done() and kos_clock_now() - start < RE_BUDGET_NS)
        {
            if (errno != EINVAL)
            {
                bad++;
            }
            reads++;
        }
        if (re_switchers_done())
        {
            g_re_finished = 1;
        }
        g_re_seen = re_rounds_total() - before;
        g_re_reads = reads;
        g_re_bad[0] = bad;
    }

    void re_switcher(void* arg)
    {
        unsigned const me = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
        re_wait_go();
        g_re_core[me + 1] = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_CORE));
        while (g_re_armed.load() == 0)
        {
            kos_yield();
        }
        uint32_t bad = 0;
        for (uint32_t i = 0; i < RE_ROUNDS; i++)
        {
            if (re_provoke_erange() != ERANGE)
            {
                bad++;
            }
            kos_yield();
            if (errno != ERANGE)
            {
                bad++;
            }
            g_re_rounds[me] = i + 1;
        }
        g_re_bad[me + 1] = bad;
        g_re_done[me] = 1;
    }

    void t_reent_per_thread_cores()
    {
        g_re_go = 0;
        g_re_armed = 0;
        g_re_reads = 0;
        g_re_seen = 0;
        g_re_finished = 0;
        for (unsigned i = 0; i < RE_SWITCHERS; i++)
        {
            g_re_rounds[i] = 0;
            g_re_done[i] = 0;
        }
        for (unsigned i = 0; i <= RE_SWITCHERS; i++)
        {
            g_re_bad[i] = 0;
            g_re_core[i] = 0xffu;
        }
        if (not pool_can_host(static_cast<int>(RE_SWITCHERS + 1)))
        {
            tap::skip("pool too small (%u concurrent workers)", RE_SWITCHERS + 1);
            return;
        }
        kos::thread::Handle w[RE_SWITCHERS + 1];
        w[0] = kos::thread::create(re_checker, nullptr, "rechk", 12);
        for (unsigned i = 0; i < RE_SWITCHERS; i++)
        {
            w[i + 1] = kos::thread::create(re_switcher,
                                           reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                                           "reswt", 12);
        }
        int pinned = 0;
        unsigned made = 0;
        for (unsigned i = 0; i <= RE_SWITCHERS; i++)
        {
            if (not w[i].valid())
            {
                continue;
            }
            made++;
            uint32_t core = RE_SWITCH_CORE;
            if (i == 0)
            {
                core = RE_CHECK_CORE;
            }
            pinned |= kos::thread::pin(w[i].id(), core);
        }
        if (made < RE_SWITCHERS + 1)
        {
            for (unsigned i = 0; i <= RE_SWITCHERS; i++)
            {
                if (w[i].valid())
                {
                    (void)w[i].kill();
                    (void)w[i].join();
                }
            }
            TAP_CHECK(made == RE_SWITCHERS + 1);
            return;
        }
        g_re_go = 1;
        int joined = 0;
        for (unsigned i = 0; i <= RE_SWITCHERS; i++)
        {
            joined |= w[i].join();
        }
        tap::diag("checker on core %u read errno %u times across %u switcher rounds on cores "
                  "%u/%u: bad checker %u, switchers %u/%u",
                  static_cast<unsigned>(g_re_core[0].load()),
                  static_cast<unsigned>(g_re_reads.load()),
                  static_cast<unsigned>(g_re_seen.load()),
                  static_cast<unsigned>(g_re_core[1].load()),
                  static_cast<unsigned>(g_re_core[2].load()),
                  static_cast<unsigned>(g_re_bad[0].load()),
                  static_cast<unsigned>(g_re_bad[1].load()),
                  static_cast<unsigned>(g_re_bad[2].load()));
        TAP_CHECK(pinned == 0);
        TAP_CHECK(joined == 0);
        // The precondition: both cores busy with threads of this task for the whole window.
        TAP_CHECK(g_re_core[0].load() == RE_CHECK_CORE);
        for (unsigned i = 1; i <= RE_SWITCHERS; i++)
        {
            TAP_CHECK(g_re_core[i].load() == RE_SWITCH_CORE);
        }
        for (unsigned i = 0; i <= RE_SWITCHERS; i++)
        {
            TAP_CHECK(g_re_bad[i].load() == 0u);
        }
        // A clean window the wall clock cut short proves the state stayed apart for fewer
        // rounds than claimed, not that it mixed.
        if (g_re_finished.load() == 0u)
        {
            TAP_SKIP_VACUOUS("the %u ms budget ended the check %u of %u switcher rounds in, "
                             "none of them bad",
                             static_cast<unsigned>(RE_BUDGET_NS / 1000000ull),
                             static_cast<unsigned>(g_re_seen.load()),
                             static_cast<unsigned>(RE_SWITCHERS * RE_ROUNDS));
            return;
        }
        TAP_CHECK(g_re_seen.load() == RE_SWITCHERS * RE_ROUNDS);
        TAP_CHECK(g_re_reads.load() > 0u);
    }
#else
    void t_reent_per_thread_cores()
    {
        tap::skip("this board has no target libc reentrant state");
    }
#endif

#if defined(__x86_64__)
    // --- The floating-point trap on every core ----------------------------------------------
    // CR0.EM makes every x87 instruction raise #NM and every SSE instruction #UD, and TS with MP
    // traps the rest of the family. The bits are per core, and SMSW reads them from ring 3
    // while CR4.UMIP is clear.
    constexpr uint32_t FP_TRAP_MSW = (1u << 1) | (1u << 2) | (1u << 3);
    Atomic<uint32_t, Order::RELAXED> g_fp_msw{0};

    void fp_msw_worker(void*)
    {
        pl_wait_go();
        uint16_t msw = 0;
        __asm__ volatile("smsw %0" : "=r"(msw));
        g_fp_msw = msw;
        g_pl_core = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_CORE));
    }

    void t_fp_trapped_every_core()
    {
        uint32_t trapped = 0;
        for (uint32_t c = 0; c < static_cast<uint32_t>(KICKOS_KERNEL_CORES); c++)
        {
            pl_reset();
            g_fp_msw = 0;
            auto w = kos::thread::create(fp_msw_worker, nullptr, "fpmsw", 12);
            if (not w.valid())
            {
                tap::skip("thread pool too small");
                return;
            }
            int const rc = kos::thread::pin(w.id(), c);
            g_pl_go = 1;
            int const joined = w.join();
            uint32_t const msw = g_fp_msw;
            uint32_t const core = g_pl_core;
            tap::diag("core %u: machine status word 0x%x, sampled on core %u",
                      static_cast<unsigned>(c), static_cast<unsigned>(msw),
                      static_cast<unsigned>(core));
            if (rc == 0 and joined == 0 and core == c and (msw & FP_TRAP_MSW) == FP_TRAP_MSW)
            {
                trapped |= 1u << c;
            }
        }
        TAP_CHECK(trapped == PL_ALL);
    }
#endif
#endif
}
