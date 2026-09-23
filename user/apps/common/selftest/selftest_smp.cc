// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The placement arms: affinity, the task core set, and the isolated cores.

#include "selftest.h"

namespace selftest
{
#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
    // --- Placement: affinity, the task's core set, and the isolated cores -----------------
    // Every scheduling probe reads the CALLER's own state, so what a worker reports is what
    // the kernel seated on that worker and not what root asked for.
    constexpr uint64_t PLACE_STEP_NS = 500000ull;
    constexpr unsigned PL_SAMPLES = 64;
    constexpr unsigned PL_DEADLINE_STEPS = 400; // PL_DEADLINE_STEPS * PLACE_STEP_NS = 200 ms
    constexpr uint32_t PL_ALL = ~0u >> (32 - KICKOS_KERNEL_CORES);

    kos_cap_t g_pl_gate = KOS_CAP_NONE; // root's release semaphore, delegated at child index 1

    // Holds a task non-empty while root asks something of it, and is released from root.
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
        // Its own mask, once: an UNPINNED spinner can be placed on the core a pin would have
        // named, so g_pl_core alone cannot tell a pin that took from one never made.
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
        int const joined = w.join(PLACE_JOIN_US);
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
        int const joined = w.join(PLACE_JOIN_US);
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
        int const joined = w.join(PLACE_JOIN_US);
        uint32_t const aff = g_pl_aff;
        uint32_t const cores = g_pl_cores;
        tap::diag("unpin returned %d: affinity 0x%x, task core set 0x%x, isolated 0x%x", rc,
                  static_cast<unsigned>(aff), static_cast<unsigned>(cores),
                  static_cast<unsigned>(iso));
        TAP_CHECK(joined == 0);
        TAP_CHECK(cores == PL_ALL);
        // Total: unpin is a mask of zero, which the kernel resolves to the task's DEFAULT set,
        // so there is nothing here for it to refuse. The DEFAULT and not the grant: the grant
        // names the isolated cores and a thread that never named one must not gain them here.
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
    // Spawns a SIBLING of its own task and places it, one task being one scheduling domain
    // whose single grant bounds the placement.
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
            (void)s.join(PLACE_JOIN_US);
        }
        (void)kos_send(1, rep, sizeof(rep));
    }

    // Runs sib_pin_worker as a member of `t` (KOS_TASK_NONE for root's own task) and collects
    // its report. False when the spawn or the rendezvous did not happen.
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
        bool const joined = m.join(PLACE_JOIN_US) == 0;
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
        // A mask of zero is the ask for the task's DEFAULT set, the same word a spawn's zero
        // core_mask carries, and not a malformed request. The malformed answer is a NONZERO
        // mask naming no core this kernel schedules: t_affinity_undriven_refused.
        int const zero = kos_thread_set_affinity(w.id(), 0);
        g_pl_stop = 1;
        int const joined = w.join(PLACE_JOIN_US);
        TAP_CHECK(joined == 0);
        TAP_CHECK(zero == 0);

        // The AUTHORITY refusal beside it, where the mask names a core the image really
        // drives: the target's task holds core 0 alone, so core 1 meets it nowhere.
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
        // A mask is a set of acceptable cores, so a bit the kernel does drive alongside one it
        // does not names the driven one and is granted. Refusing it would make all ones a
        // magic value rather than an ordinary request for the whole grant.
        int const mixed = kos_thread_set_affinity(w.id(), undriven | 0x1u);
        int const good = kos_thread_set_affinity(w.id(), 0x1u);
        g_pl_stop = 1;
        int const joined = w.join(PLACE_JOIN_US);
        TAP_CHECK(joined == 0);
        TAP_CHECK(bad == -KOS_EINVAL);
        TAP_CHECK(mixed == 0);
        TAP_CHECK(good == 0);
    }

    // --- Placement: a running thread is re-placed under an affinity change ---------------
    // The move is driven by a pinned child: root holds no handle to itself, so it cannot be
    // kept off the core the spinner is measured on. The spinner sits above root's priority and
    // the driver above the spinner, which keeps the driver scheduled once the two share the
    // destination.
    constexpr uint32_t PL_HOME = 0; // the boot core, this arm's destination
    // The driver spends BOTH deadlines before it exits, so root's wait on it covers them plus
    // an ordinary join budget.
    constexpr uint32_t PL_DRIVER_JOIN_US =
        PLACE_JOIN_US
        + 2u * static_cast<uint32_t>((PL_DEADLINE_STEPS * PLACE_STEP_NS) / 1000ull);
    kos_thread_t g_pl_victim = KOS_THREAD_NONE;
    Atomic<uint32_t, Order::RELAXED> g_pl_mig_first{0xffu};
    Atomic<uint32_t, Order::RELAXED> g_pl_mig_last{0xffu};
    Atomic<uint32_t, Order::RELAXED> g_pl_mig_arrived{0};
    Atomic<uint32_t, Order::RELAXED> g_pl_mig_rc{0x7fffffffu};

    void pl_migrate_driver(void*)
    {
        pl_wait_go();
        // Wait for the spinner to publish at all before moving it: it was pinned to the source
        // at creation, so any value it publishes is that core, and waiting turns a slow start
        // into a red precondition instead of a lost race.
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
        // The boot core is the DESTINATION: it can never be isolated, so a two-core part has
        // one. The source is the lowest core above it the isolated mask does not name; a
        // re-placement onto an isolated core is a different claim and no arm makes it.
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
        // Before the driver exists, so the relaxed cells owe no publication order.
        g_pl_victim = w.id();
        auto d = kos::thread::create(pl_migrate_driver, nullptr, "migrd", 13);
        if (not d.valid())
        {
            g_pl_stop = 1;
            (void)w.join(PLACE_JOIN_US);
            tap::skip("thread pool too small");
            return;
        }
        int const dpin = kos::thread::pin(d.id(), PL_HOME);
        g_pl_go = 1;
        int const djoined = d.join(PL_DRIVER_JOIN_US);
        int const joined = w.join(PLACE_JOIN_US);
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
        // The precondition: a core running this thread and nothing else was made to give it up.
        TAP_CHECK(aff == (1u << away));
        TAP_CHECK(first == away);
        TAP_CHECK(rc == 0);
        // A deadline and not a park: a worker that never arrives fails here instead of
        // hanging the suite.
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
            mj = m.join(PLACE_JOIN_US);
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(g_pl_gate);
        TAP_CHECK(seated);
        TAP_CHECK(mj == 0);
        // Root is unprivileged here, and a core its OWN grant holds does not make the target
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
            mj = m.join(PLACE_JOIN_US);
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(g_pl_ep);
        TAP_CHECK(granted == 0);
        TAP_CHECK(seated);
        TAP_CHECK(heard);
        TAP_CHECK(mj == 0);
        TAP_CHECK(rep[GN_CORES] == 0x3);
        // The member's affinity is seated FROM the set, so a grant nothing re-derived would
        // show here as a thread wider than its own task.
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
            mj = m.join(PLACE_JOIN_US);
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
    // The caller-side check in task_sched_grant weighs a request against the CALLER's grant,
    // which root holds whole, so only task_sched_narrow's own check against the TASK's current
    // set can refuse this.
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
    // grant, so a task that wrongly defaulted to the whole machine is invisible until a thread
    // of it reports the set it actually got.
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
        (void)c.join(PLACE_JOIN_US);
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
            (void)m.join(PLACE_JOIN_US);
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
            mj = m.join(PLACE_JOIN_US);
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(g_pl_gate);
        TAP_CHECK(empty_ok == 0);
        TAP_CHECK(seated);
        TAP_CHECK(mj == 0);
        // Thread::affinity is a subset of the set and nothing re-derives it, so a grant that
        // narrowed under a live member would strand it.
        TAP_CHECK(busy == -KOS_EBUSY);
    }

    // A task granted exactly one isolated core. Its grant names nothing else, so the default
    // set is the grant and an unpinned member is pinned by construction. Only a member can show
    // it, the set a task really got being invisible from outside.
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
            mj = m.join(PLACE_JOIN_US);
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

    // An exited-but-unreclaimed slot still gen-matches, so the handle resolves and there is
    // nothing left in it to place. Kill and slay answer the same way.
    void t_affinity_dead_handle_refused()
    {
        auto w = kos::thread::create(pl_exit_worker, nullptr, "adead", 9);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        int const joined = w.join(PLACE_JOIN_US);
        int const dead = kos_thread_set_affinity(w.id(), 0x1u);
        TAP_CHECK(joined == 0);
        TAP_CHECK(dead == -KOS_EBADF);
    }

    // The drift mechanism, read at the mask and then at the cores. A thread that names no core
    // is given the task's set less the isolated ones, so it arrives on none of them; the cores
    // it was actually seen on are the second half, a mask that excluded them proving nothing on
    // its own if the thread never ran. It says nothing about a thread that DOES name one:
    // t_isolated_takes_pinned and t_isolated_mixed_mask_ok are that half.
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
        int const joined = w.join(PLACE_JOIN_US);
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

    // The same mechanism read at the other end: a thread that was pinned to the isolated core
    // and is then unpinned. Unpin restores the DEFAULT set and not the grant, so the isolated
    // core is gone from the mask; a thread widened to the whole grant instead would keep the
    // core it already holds and go on running there. Both channels are read, the mask and the
    // cores the thread was actually seen on, the mask alone proving nothing if it never ran.
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
        int const joined = w.join(PLACE_JOIN_US);
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
        int const joined = w.join(PLACE_JOIN_US);
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

    // The other half of the opt-in, and the one the guarantee's wording turns on: an isolated
    // core named beside an ordinary one. The mask is admitted whole and lands verbatim, so
    // that core's picker holds the thread like any other core in the set. Which of the two it
    // is seen on is the picker's and is not asserted; what is asserted is that it ran, and
    // never outside the mask.
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
        int const joined = w.join(PLACE_JOIN_US);
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
    // KOS_SCHED_OP_PREEMPTED is machine-wide, which is what a claim about a SECONDARY arming
    // its own comparator needs.
    //
    // The probe records an expiry only where the tick changed the running thread
    // (kernel/time/time.cc brackets sched::tick_rr and compares sched::current()). An expiry
    // on a core carrying ONE runnable thread of its priority rotates the incumbent to itself
    // and records nothing, however long that thread runs. So the claim needs an equal-priority
    // PEER queued on the core under test, and leaving that to the scheduler made the arm
    // undecidable: a loaded host settles an unpinned crowd one-per-core, and a core that never
    // carried a pair then reads exactly like a comparator that never fired. Every quantity an
    // unpinned crowd can observe (which cores it reached, how many of it reached them, how
    // long it held them in wall clock or in work) reads the SAME in both cases.
    //
    // So the pair is placed rather than hoped for: one ROUND per core, with two burners pinned
    // to the core under test and one pinned to each of the others. The peer is then there by
    // construction and only the kernel's own comparator decides the outcome, which is what
    // makes the decline below decidable.
    //
    // What this gives up. The unpinned crowd also exercised PLACEMENT for free: it asserted,
    // incidentally, that the scheduler spreads a crowd over every core. It does not any more.
    // That claim is threads_reach_every_core's subject and is asserted there; nothing about
    // placement is claimed here, and this arm passing says nothing about it.
    //
    // One thread MORE than the machine has cores, unchanged: two on the core under test and
    // one on each of the rest is exactly that many, so no board's thread budget moves.
    constexpr unsigned PL_CROWD = KICKOS_KERNEL_CORES + 1u;
    constexpr uint64_t PL_BURN_QUANTA = 8ull; // per round, so a slice has room to expire several times

    // Read-only to the burners, published before the first one is created.
    uint64_t g_pl_burn_ns = 32000000ull;

    // What each burner saw of itself. `first`/`last` are the low 32 bits of the clock at its
    // first and last sample: both are instants at which that thread was observed executing, so
    // the span between them is not a wall clock the host can inflate behind its back. A span
    // reaching one quantum is the decidable part of the claim, because the deadline armed at
    // that thread's switch-in falls inside it, and the closing sample cannot have run unless
    // the core took the interrupt that deadline had already made due.
    Atomic<uint32_t, Order::RELAXED> g_pl_burn_seen[PL_CROWD];
    Atomic<uint32_t, Order::RELAXED> g_pl_burn_first[PL_CROWD];
    Atomic<uint32_t, Order::RELAXED> g_pl_burn_last[PL_CROWD];
    Atomic<uint32_t, Order::RELAXED> g_pl_burn_samples[PL_CROWD];

    // Spins, never yields or sleeps: the slice has to EXPIRE under this thread for the timer
    // to be what takes the core away. The core sample is one extra syscall every 16 passes
    // beside the clock read the loop already makes, and it yields nothing.
    void pl_burn_worker(void* arg)
    {
        unsigned const me = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
        // Released together, and this is what makes the peer a construction rather than a
        // hope. A burner starts running the instant it is created, so without the gate the
        // first of a pair is already burning while root is still creating the second, and on
        // a host slow enough that the gap outlasts the round the core never carries two of
        // ours: every expiry rotates the incumbent to itself and records nothing, which is
        // the defect's own signature. Both wake from their own sleep at the release, so both
        // are on the core's run queue before either has armed a slice.
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
        // An isolated core is held out of the default core set and these burners name no core,
        // so it is outside the claim. Core 0 can never be isolated, so the target is never empty.
        uint32_t const want = PL_ALL & ~iso;
        // Asked before the baseline read below, so the probe's own threads contribute no bit
        // to `after`.
        if (not pool_can_host(static_cast<int>(PL_CROWD)))
        {
            tap::skip("pool too small (%u concurrent burners)", PL_CROWD);
            return;
        }
        uint32_t const before = static_cast<uint32_t>(kos_sched_probe(KOS_SCHED_OP_PREEMPTED));

        // The quantum must be resolvable by the monotonic clock or no slice can expire under a
        // burn at all, so it is measured: an emulated clock's granule is coarse. That also makes
        // this the suite's most load-dependent arm, a contended host leaving the burners fewer
        // rotations than the claim needs.
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
        uint64_t quantum = 1000000ull; // 1 ms on a fine clock (the shipped case)
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
                // Already recorded, and the mask never clears, so this round could add nothing
                // to it. A bit can only be there because that core's own comparator changed the
                // thread it was running, which is the whole claim.
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
                // Burners 0 and 1 ARE the claim: an equal-priority pair on the core under test,
                // so its next expiry has somewhere to switch. The rest hold the other cores so
                // the machine is loaded as it was, and nothing is asserted of them.
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
                int rc = w[i].join(PLACE_JOIN_US);
                if (rc != 0)
                {
                    // Once more before giving up: the burn is bounded in GUEST time and the
                    // burner exits of its own accord, so a first budget spent under load is
                    // usually only that.
                    rc = w[i].join(PLACE_JOIN_US);
                }
                joined |= rc;
            }
            // The probe at the top just held PL_CROWD slots and stacks, so a short crowd here
            // is a pool bug and not a small board. Checked before the next round reuses them.
            TAP_CHECK(made == PL_CROWD);
            if (joined != 0)
            {
                // The round never finished, so nothing about this core was established, and
                // what ran out is wall clock: the burn is bounded in guest time and a burner
                // the host does not give a core to cannot reach its own exit. The stragglers
                // go before the next round asks for their slots back.
                for (unsigned i = 0; i < made; i++)
                {
                    (void)w[i].slay(PLACE_JOIN_US);
                }
                // Short enough that the assembled TAP line clears tap.cc's 224-byte emitf
                // buffer: a reason that overflows it loses its own newline and swallows the
                // next line whole.
                tap::skip_vacuous("core %u's round outlasted two %u ms join budgets, so its "
                                  "pinned pair was never given the core",
                                  static_cast<unsigned>(k),
                                  static_cast<unsigned>(PLACE_JOIN_US / 1000u));
                return;
            }

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
        // The pair went somewhere it was not given, so the round staged something else.
        TAP_CHECK(strayed == 0u);
        // The claim, and it is asserted only where the round gave that core's pair a whole
        // quantum of the core: a pair that never got one had no expiry to be taken off by, and
        // that is a window the host took rather than a comparator left unarmed.
        uint32_t const denied = unproven & ~starved;
        TAP_CHECK(denied == 0u);
        if (unproven != 0u)
        {
            tap::skip_vacuous("no preemption on core(s) 0x%x, and their pinned pair never held "
                              "the core a whole %u us quantum: the window went, not the "
                              "comparator",
                              static_cast<unsigned>(starved),
                              static_cast<unsigned>(quantum / 1000u));
            return;
        }
    }

    // --- Placement: a thread of this app is seen running on every core --------------------
    // The union across an unpinned crowd has to NAME every core the crowd may run on, which no
    // single-core placement satisfies. isolated_unpinned_never reads the same probe for an
    // absence, which a scheduler that moves nobody also satisfies.
    //
    // One cell per worker: a shared mask is a cross-core read-modify-write, and a lost update
    // erases a core from the very union being read.
    Atomic<uint32_t, Order::RELAXED> g_pl_spread[PL_CROWD];
    Atomic<uint32_t, Order::RELAXED> g_pl_spread_aff[PL_CROWD];
    // Samples taken, so a short union can be told apart from a starved one: the budget below
    // is wall clock, and a loaded host spends it without the crowd being offered the
    // rotations the union needs.
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
        // Asked before the first spawn, for the reason the shortfall check below states.
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
                (void)w[i].join(PLACE_JOIN_US);
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
            joined |= w[i].join(PLACE_JOIN_US);
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
        // One pass is one sample and one yield, so a worker needs at least one pass per core
        // in the target set to be SEEN on all of them. Four times that is the floor here: it
        // leaves the placement three opportunities per core beyond the minimum and still
        // refuses a crowd that was given none.
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
        // The budget is wall clock and the claim is about placement, so a short union has two
        // causes. Only the one where the crowd was given its rotations is a placement
        // failure; the other is the host, and it is declined by name.
        if ((reached & want) != want and fewest < floor_passes)
        {
            tap::skip_vacuous("the %u ms placement budget ran out with the slowest worker at "
                              "%u sample(s) against the %u this asks, so the crowd reached "
                              "0x%x of 0x%x for want of rotations, not for want of placement",
                              static_cast<unsigned>(PL_SPREAD_BUDGET_NS / 1000000ull),
                              static_cast<unsigned>(fewest), static_cast<unsigned>(floor_passes),
                              static_cast<unsigned>(reached), static_cast<unsigned>(want));
            return;
        }
        TAP_CHECK((reached & want) == want);
    }

    // A fused reply followed by a park must notify the caller's core.
    // A FIFO spinner occupies that core, so only a reschedule request lets the
    // higher-priority caller run. Single-core builds cannot exercise this path.
    constexpr uint8_t XC_SPIN_PRIO = 14;   // above root, below the caller
    constexpr uint8_t XC_CALLER_PRIO = 20; // the thread the reply readies
    constexpr uint8_t XC_SERVER_PRIO = 12;
    constexpr uint32_t XC_JOIN_US = 400000;
    kos_cap_t g_xc_ep = KOS_CAP_NONE;
    kos_cap_t g_xc_gate = KOS_CAP_NONE;
    Atomic<uint32_t, Order::RELAXED> g_xc_stop{0};
    Atomic<uint32_t, Order::RELAXED> g_xc_spins{0};
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
        // Wait until the caller is parked for its reply and the spinner holds
        // the caller's core before replying.
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
    void xc_spinner(void*) // caps: gate@1
    {
        kos_sem_post(1); // the core is ours; the server may answer now
        uint32_t n = 0;
        while (g_xc_stop.load() == 0)
        {
            n = n + 1;
            g_xc_spins = n;
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
        g_xc_spins = 0;
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
                (void)sp.join(XC_JOIN_US);
            }
            (void)kos_handle_close(g_xc_ep);
            kos_sem_destroy(g_xc_gate);
            tap::skip("pool too small for 3 threads");
            return;
        }
        // Bound the wait so a missing peer request fails instead of hanging.
        int const cjoined = cl.join(XC_JOIN_US);
        g_xc_stop = 1;
        int const sjoined = sp.join(XC_JOIN_US);
        // Release the server from its final receive.
        (void)kos_send(g_xc_ep, "", 0);
        int const vjoined = sv.join(XC_JOIN_US);
        (void)kos_handle_close(g_xc_ep);
        kos_sem_destroy(g_xc_gate);
        tap::diag("caller pinned to core %u under a spinner: join %d, call %d, spins %u",
                  static_cast<unsigned>(away), cjoined,
                  static_cast<int>(g_xc_call_rc.load()),
                  static_cast<unsigned>(g_xc_spins.load()));
        TAP_CHECK(sjoined == 0);
        TAP_CHECK(vjoined == 0);
        // Verify that the spinner held the caller's core when the reply was sent.
        TAP_CHECK(g_xc_spins.load() > 0u);
        TAP_CHECK(cjoined == 0);
        // Check the reply value as well as caller completion.
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
    constexpr uint32_t XIRQ_JOIN_US = 2000000u;
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
            ijoined = inj.join(XIRQ_JOIN_US);
        }
        wait_n(1);
        int const wjoined = w.join(XIRQ_JOIN_US);
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
        (void)t.join(XIRQ_JOIN_US);
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
        int const ojoined = old.join(XIRQ_JOIN_US);
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
#endif
}
