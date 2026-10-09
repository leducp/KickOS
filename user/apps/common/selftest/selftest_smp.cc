// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The multi-core arms: placement, slice preemption, cross-core wakes and IRQs, and libc state.

#include "selftest.h"

#include <kickos/config/priorities.h>

#include <errno.h>
#include <stdlib.h>

#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1 && defined(__x86_64__)
// XCR0, in assembly of its own: tests/static/check_x86_64_no_vector.sh admits the vector-state
// instructions of the selftest by the name of the function carrying them.
extern "C" KICKOS_SELFTEST_LOCAL uint64_t selftest_vec_xcr0(void);
__asm__(".pushsection .text, \"ax\", @progbits\n"
        "    .balign 16\n"
        "    .globl selftest_vec_xcr0\n"
        "    .hidden selftest_vec_xcr0\n"
        "    .type selftest_vec_xcr0, @function\n"
        "selftest_vec_xcr0:\n"
        "    xorl %ecx, %ecx\n"
        "    xgetbv\n"
        "    shlq $32, %rdx\n"
        "    orq %rdx, %rax\n"
        "    ret\n"
        ".popsection\n");
#endif

namespace selftest
{
#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
    // --- Placement: affinity, the task's core set, and the isolated cores -----------------
    // A worker reads its own placement: what the kernel seated on it, not what main asked for.
    constexpr uint64_t PLACE_STEP_NS = 500000ull;
    constexpr unsigned PL_SAMPLES = 64;
    constexpr uint32_t PL_ALL = ~0u >> (32 - KICKOS_KERNEL_CORES);

    kos_cap_t g_pl_gate = KOS_CAP_NONE;

    // Keeps its task non-empty until main posts the gate.
    void pl_gate_worker(void*) // caps: gate@1
    {
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
    }

    Atomic<uint32_t, Order::RELAXED> g_pl_go{0};
    Atomic<uint32_t, Order::RELAXED> g_pl_stop{0};
    Atomic<uint32_t, Order::RELAXED> g_pl_core{0xffu};
    Atomic<uint32_t, Order::RELAXED> g_pl_seen{0};
    Atomic<uint32_t, Order::RELAXED> g_pl_aff{0};
    Atomic<uint32_t, Order::RELAXED> g_pl_cores{0};
    Atomic<int32_t, Order::RELAXED> g_pl_read{0}; // main's read of the worker's affinity

    void pl_reset()
    {
        g_pl_go = 0;
        g_pl_stop = 0;
        g_pl_core = 0xffu;
        g_pl_seen = 0;
        g_pl_aff = 0;
        g_pl_cores = 0;
        g_pl_read = 0;
    }

    // Bit 31 names no core: a kernel drives at most 31.
    constexpr uint32_t PL_NO_CORE = 31;

    uint32_t pl_core()
    {
        int const c = kos_core_current();
        if (c < 0 or c >= static_cast<int>(KICKOS_KERNEL_CORES))
        {
            return PL_NO_CORE;
        }
        return static_cast<uint32_t>(c);
    }

    uint32_t pl_affinity()
    {
        return static_cast<uint32_t>(kos_thread_affinity(kos_thread_self()));
    }

    uint32_t pl_task_cores()
    {
        return static_cast<uint32_t>(kos_task_cores(KOS_TASK_NONE));
    }

    // The gate SLEEPS rather than yields: a worker above main's priority that spun here would
    // hold the core main has to run on to release it.
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
        g_pl_aff = pl_affinity();
        g_pl_cores = pl_task_cores();
        uint32_t seen = 0;
        for (unsigned i = 0; i < PL_SAMPLES; i++)
        {
            uint32_t const c = pl_core();
            g_pl_core = c;
            seen |= 1u << c;
            kos_yield();
        }
        g_pl_seen = seen;
    }

    // pl_sample_worker created on `mask` (0: the task's default set), its affinity set to `to`
    // before it samples where `place`, and joined: g_pl_* hold what it read, *placed the setter's
    // answer and g_pl_read main's read of the live worker's affinity after it.
    void pl_sample(uint32_t mask, bool place, uint32_t to, int* placed)
    {
        pl_reset();
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create_caps(pl_sample_worker, nullptr, "plsmp", 12, nullptr, 0,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                     KOS_TASK_NONE, nullptr, 0, mask);
        TAP_CHECK(w.valid());
        if (place)
        {
            *placed = kos_thread_set_affinity(w.id(), to);
            g_pl_read = kos_thread_affinity(w.id());
        }
        g_pl_go = 1;
        TAP_CHECK(hold.joined());
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
        g_pl_aff = pl_affinity();
        while (g_pl_stop.load() == 0)
        {
            g_pl_core = pl_core();
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

    // Seats `entry` with g_pl_ep's `rights` at index 1 and takes the report it sends there: in a
    // fresh task granted `cores`, the grant's answer in *granted, or in main's own when `cores` is
    // 0.
    void pl_member_report(void (*entry)(void*), uint32_t cores, uint8_t rights,
                          uint32_t authority, void* rep, size_t len, int* granted)
    {
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle m;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_pl_ep) and hold.task(&t) and hold.thread(&m));
        TAP_CHECK(kos_endpoint_create(&g_pl_ep) == 0);
        if (cores != 0)
        {
            TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
            *granted = kos_task_sched_grant(t, 0, cores);
        }
        kos_cap_grant caps[] = {{g_pl_ep, rights}};
        m = kos::thread::create_caps(entry, nullptr, "plmem", 11, caps, 1, KOS_POLICY_FIFO, 0,
                                     false, nullptr, 0, authority, nullptr, t);
        TAP_CHECK(m.valid());
        TAP_CHECK(report_await(g_pl_ep, rep, len));
        TAP_CHECK(hold.joined());
    }

    // Core 1, then the last core.
    void t_pin_places()
    {
        TAP_ASK(.workers = 1);
        uint32_t const last = static_cast<uint32_t>(KICKOS_KERNEL_CORES) - 1u;
        uint32_t core = 1u;
        while (true)
        {
            int rc = -99;
            pl_sample(0, true, 1u << core, &rc);
            int32_t const read = g_pl_read;
            uint32_t const aff = g_pl_aff;
            uint32_t const seen = g_pl_seen;
            tap::diag("pinned to core %u: read 0x%x, own affinity 0x%x, %u samples across a yield"
                      " each: cores 0x%x",
                      static_cast<unsigned>(core), static_cast<unsigned>(read),
                      static_cast<unsigned>(aff), PL_SAMPLES, static_cast<unsigned>(seen));
            TAP_CHECK(rc == 0);
            TAP_CHECK(read == static_cast<int32_t>(1u << core));
            TAP_CHECK(aff == (1u << core));
            TAP_CHECK(seen == (1u << core));
            if (core == last)
            {
                break;
            }
            core = last;
        }
    }

    void t_unpin_restores()
    {
        TAP_ASK(.workers = 1);
        uint32_t const iso = KOS_ISOLATED_CORES;
        int rc = -99;
        pl_sample(1u << 1, true, 0, &rc);
        uint32_t const aff = g_pl_aff;
        uint32_t const cores = g_pl_cores;
        tap::diag("unpin returned %d: affinity 0x%x, task core set 0x%x, isolated 0x%x", rc,
                  static_cast<unsigned>(aff), static_cast<unsigned>(cores),
                  static_cast<unsigned>(iso));
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
        rep[XS_CORES] = static_cast<int32_t>(kos_task_cores(KOS_TASK_NONE));
        g_pl_stop = 0;
        auto s = kos::thread::create(pl_park_worker, nullptr, "sibpk", 9);
        if (s.valid())
        {
            rep[XS_MADE] = 1;
            rep[XS_ONE] = kos::thread::pin(s.id(), 1);
            rep[XS_ZERO] = kos::thread::pin(s.id(), 0);
            g_pl_stop = 1;
            (void)s.join(STALL_TOLERANT_US);
        }
        (void)kos_send(1, rep, sizeof(rep));
    }

    void t_pin_beyond_grant_refused()
    {
        TAP_ASK(.workers = 2, .tasks = 1, .endpoints = 1);
        pl_reset();
        // The authority refusal: the task holds core 0 alone, so a pin to core 1, a core the
        // image drives, is refused.
        int granted = -99;
        int32_t rep[XS_WORDS] = {0, 0, 0, 0};
        pl_member_report(sib_pin_worker, 0x1, KOS_CAP_SIGNAL, 0, rep, sizeof(rep), &granted);
        TAP_CHECK(granted == 0);
        TAP_CHECK(rep[XS_CORES] == 0x1);
        TAP_CHECK(rep[XS_MADE] == 1);
        TAP_CHECK(rep[XS_ONE] == -KOS_EPERM);
        TAP_CHECK(rep[XS_ZERO] == 0);
    }

    void t_affinity_undriven_refused()
    {
        TAP_ASK(.workers = 1);
        pl_reset();
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create(pl_park_worker, nullptr, "aundr", 9);
        TAP_CHECK(w.valid());
        uint32_t const undriven = 1u << static_cast<uint32_t>(KICKOS_KERNEL_CORES);
        int const bad = kos_thread_set_affinity(w.id(), undriven);
        // A mask is a set of acceptable cores: an undriven bit beside a driven one names the
        // driven one and is granted, so all ones is an ordinary request for the whole grant.
        int const mixed = kos_thread_set_affinity(w.id(), undriven | 0x1u);
        int const good = kos_thread_set_affinity(w.id(), 0x1u);
        g_pl_stop = 1;
        TAP_CHECK(hold.joined());
        TAP_CHECK(bad == -KOS_EINVAL);
        TAP_CHECK(mixed == 0);
        TAP_CHECK(good == 0);
    }

    // --- Placement: a running thread is re-placed under an affinity change ---------------
    // A pinned child drives the move: main holds no handle to itself, so it cannot be kept off
    // the spinner's core. The spinner sits above main and the driver above the spinner, so the
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
        // Pinned to the source at creation, the spinner's first publish is that core, so a slow
        // start fails the precondition instead of losing a race.
        (void)flag_await_change(g_pl_core, 0xffu);
        g_pl_mig_first = g_pl_core.load();
        int const rc = kos::thread::pin(g_pl_victim, PL_HOME);
        g_pl_mig_rc = static_cast<uint32_t>(rc);
        if (flag_await(g_pl_core, PL_HOME))
        {
            g_pl_mig_arrived = 1;
        }
        g_pl_mig_last = g_pl_core.load();
        g_pl_stop = 1;
    }

    void t_migrate_running()
    {
        uint32_t const iso = KOS_ISOLATED_CORES;
        // The boot core is the destination because it can never be isolated. The source must not
        // be isolated either: a move involving an isolated core is a different claim.
        uint32_t away = 0;
        for (uint32_t c = 1; c < static_cast<uint32_t>(KICKOS_KERNEL_CORES); c++)
        {
            if ((iso & (1u << c)) == 0)
            {
                away = c;
                break;
            }
        }
        TAP_SKIP_UNLESS(away != 0, "a migration needs a non-isolated core beside the boot core");
        TAP_ASK(.workers = 2);
        pl_reset();
        g_pl_mig_first = 0xffu;
        g_pl_mig_last = 0xffu;
        g_pl_mig_arrived = 0;
        g_pl_mig_rc = 0x7fffffffu;
        kos::thread::Handle w;
        kos::thread::Handle d;
        ArmHold hold(2u * STALL_TOLERANT_US);
        TAP_HOLD(hold.thread(&w) and hold.thread(&d));
        w = kos::thread::create_caps(pl_spin_worker, nullptr, "migr", 12, nullptr, 0,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                     KOS_TASK_NONE, nullptr, 0, 1u << away);
        TAP_CHECK(w.valid());
        // Set before the driver exists, so the relaxed cell needs no publication order.
        g_pl_victim = w.id();
        d = kos::thread::create(pl_migrate_driver, nullptr, "migrd", 13);
        TAP_CHECK(d.valid());
        int const dpin = kos::thread::pin(d.id(), PL_HOME);
        g_pl_go = 1;
        TAP_CHECK(hold.joined());
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
        // The precondition: the spinner ran pinned to the source before the move.
        TAP_CHECK(aff == (1u << away));
        TAP_CHECK(first == away);
        TAP_CHECK(rc == 0);
        TAP_CHECK(g_pl_mig_arrived.load() == 1u);
    }

    void t_pin_cross_task_refused()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .sems = 1);
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle m;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_pl_gate) and hold.task(&t) and hold.thread(&m));
        TAP_CHECK(kos_sem_create(0, &g_pl_gate) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        kos_cap_grant caps[] = {{g_pl_gate, CH_FULL}};
        m = kos::thread::create_caps(pl_gate_worker, nullptr, "xtask", 9, caps, 1,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr, t);
        TAP_CHECK(m.valid());
        int const rc = kos_thread_set_affinity(m.id(), 0x1u);
        kos_sem_post(g_pl_gate);
        TAP_CHECK(hold.joined());
        // main is unprivileged: a core its OWN grant holds does not make another task's thread
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
        rep[GN_CORES] = static_cast<int32_t>(kos_task_cores(KOS_TASK_NONE));
        rep[GN_AFF] = static_cast<int32_t>(kos_thread_affinity(kos_thread_self()));
        rep[GN_CORE] = static_cast<int32_t>(kos_core_current());
        (void)kos_send(1, rep, sizeof(rep));
    }

    void t_grant_narrows()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .endpoints = 1);
        int granted = -99;
        int32_t rep[GN_WORDS] = {0, 0, 0};
        pl_member_report(gn_worker, 0x3, KOS_CAP_SIGNAL, 0, rep, sizeof(rep), &granted);
        TAP_CHECK(granted == 0);
        TAP_CHECK(rep[GN_CORES] == 0x3);
        // Affinity is seated FROM the set, so a grant nothing re-derived shows as a thread wider
        // than its own task.
        TAP_CHECK(rep[GN_AFF] == 0x3);
        TAP_CHECK((0x3 & (1 << rep[GN_CORE])) != 0);
    }

    // --- A second grant narrows again and never re-widens -------------------------------
    // task_sched_grant weighs a request against the CALLER's grant, which main holds whole, so
    // only task_sched_narrow's check against the TASK's current set can refuse this.
    void t_grant_second_narrow_only()
    {
        TAP_SKIP_UNLESS(PL_ALL != 0x1u, "re-widening needs a set wider than core 0 to ask for");
        TAP_ASK(.tasks = 1);
        kos_task_t t = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.task(&t));
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        int const first = kos_task_sched_grant(t, 0, 0x1);
        int const rewiden = kos_task_sched_grant(t, 0, PL_ALL);
        int const again = kos_task_sched_grant(t, 0, 0x1);
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
                                 static_cast<int32_t>(kos_task_cores(KOS_TASK_NONE))};
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
        (void)c.join(STALL_TOLERANT_US);
        (void)kos_task_kill(u);
    }
    void t_grant_inherited_by_child_task()
    {
        TAP_SKIP_UNLESS(PL_ALL != 0x1u,
                        "inheritance needs a set narrower than the whole machine to inherit");
        TAP_ASK(.workers = 2, .tasks = 2, .endpoints = 1);
        int granted = -99;
        int32_t rep[GI_WORDS] = {0, 0, -1};
        // TRANSFER as well as SIGNAL: the worker hands this endpoint on to the member it seats
        // into the nested task, and that member is what reports the inherited set. The member
        // creates that task, so it holds the task authority.
        pl_member_report(gi_worker, 0x1, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER, KOS_AUTH_TASKS, rep,
                         sizeof(rep), &granted);
        tap::diag("nested task made %d, member seated %d, cores/err %d",
                  static_cast<int>(rep[GI_MADE]), static_cast<int>(rep[GI_SEATED]),
                  static_cast<int>(rep[GI_CORES]));
        TAP_CHECK(granted == 0);
        TAP_CHECK(rep[GI_MADE] == 1);
        TAP_CHECK(rep[GI_SEATED] == 1);
        // 0x1 and not PL_ALL: a task that did not inherit reads as the whole machine.
        TAP_CHECK(rep[GI_CORES] == 0x1);
    }

    void t_grant_after_member_refused()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .sems = 1);
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle m;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_pl_gate) and hold.task(&t) and hold.thread(&m));
        TAP_CHECK(kos_sem_create(0, &g_pl_gate) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        int const empty_ok = kos_task_sched_grant(t, 0, 0x3);
        kos_cap_grant caps[] = {{g_pl_gate, CH_FULL}};
        m = kos::thread::create_caps(pl_gate_worker, nullptr, "gbusy", 9, caps, 1,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr, t);
        TAP_CHECK(m.valid());
        int const busy = kos_task_sched_grant(t, 0, 0x1);
        kos_sem_post(g_pl_gate);
        TAP_CHECK(hold.joined());
        TAP_CHECK(empty_ok == 0);
        // Thread::affinity is a subset of the set and nothing re-derives it, so narrowing under
        // a live member would strand it.
        TAP_CHECK(busy == -KOS_EBUSY);
    }

    // A grant of exactly one isolated core and nothing else: the default set is then the grant,
    // so an unpinned member runs on the isolated core.
    void t_isolated_single_grant_ok()
    {
        uint32_t const iso = KOS_ISOLATED_CORES;
        TAP_SKIP_UNLESS(iso != 0, "this image isolates no core");
        TAP_ASK(.workers = 1, .tasks = 1, .endpoints = 1);
        uint32_t const core = pl_lowest(iso);
        uint32_t const bit = 1u << core;
        int granted = -99;
        int32_t rep[GN_WORDS] = {0, 0, 0};
        pl_member_report(gn_worker, bit, KOS_CAP_SIGNAL, 0, rep, sizeof(rep), &granted);
        tap::diag("granted isolated core %u alone: task cores 0x%x, affinity 0x%x, ran on %d",
                  static_cast<unsigned>(core), static_cast<unsigned>(rep[GN_CORES]),
                  static_cast<unsigned>(rep[GN_AFF]), static_cast<int>(rep[GN_CORE]));
        TAP_CHECK(granted == 0);
        TAP_CHECK(rep[GN_CORES] == static_cast<int32_t>(bit));
        TAP_CHECK(rep[GN_AFF] == static_cast<int32_t>(bit));
        TAP_CHECK(rep[GN_CORE] == static_cast<int32_t>(core));
    }

    void pl_exit_worker(void*)
    {
    }

    // An exited but unreclaimed slot still gen-matches, so the handle resolves to nothing left
    // to place or read; kill and slay give the same answer.
    void t_affinity_dead_handle_refused()
    {
        TAP_ASK(.workers = 1);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create(pl_exit_worker, nullptr, "adead", 9);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(kos_thread_set_affinity(w.id(), 0x1u) == -KOS_EBADF);
        TAP_CHECK(kos::thread::affinity(w.id()) == -KOS_EBADF);
        TAP_CHECK(kos_task_cores(0xFFFFFFFFu) == -KOS_EBADF);
    }

    // A thread that names no core is given the task's set less the isolated cores.
    void t_isolated_unpinned_never()
    {
        uint32_t const iso = KOS_ISOLATED_CORES;
        TAP_SKIP_UNLESS(iso != 0, "this image isolates no core");
        TAP_ASK(.workers = 1);
        int unused = 0;
        pl_sample(0, false, 0, &unused);
        uint32_t const aff = g_pl_aff;
        uint32_t const seen = g_pl_seen;
        tap::diag("unpinned worker: affinity 0x%x, cores seen 0x%x, isolated 0x%x",
                  static_cast<unsigned>(aff), static_cast<unsigned>(seen),
                  static_cast<unsigned>(iso));
        TAP_CHECK(aff == (PL_ALL & ~iso));
        TAP_CHECK(seen != 0); // the denominator: a worker that never ran would satisfy the rest
        TAP_CHECK((seen & iso) == 0);
    }

    // Unpin off an isolated core restores the DEFAULT set and not the grant: widened to the
    // grant, the thread would keep running on the isolated core it holds.
    void t_isolated_unpin_excludes()
    {
        uint32_t const iso = KOS_ISOLATED_CORES;
        TAP_SKIP_UNLESS(iso != 0, "this image isolates no core");
        TAP_ASK(.workers = 1);
        uint32_t const core = pl_lowest(iso);
        int rc = -99;
        pl_sample(1u << core, true, 0, &rc);
        uint32_t const aff = g_pl_aff;
        uint32_t const seen = g_pl_seen;
        tap::diag("unpinned off isolated core %u: affinity 0x%x, cores seen 0x%x, isolated 0x%x",
                  static_cast<unsigned>(core), static_cast<unsigned>(aff),
                  static_cast<unsigned>(seen), static_cast<unsigned>(iso));
        TAP_CHECK(rc == 0);
        TAP_CHECK(aff == (PL_ALL & ~iso));
        TAP_CHECK(seen != 0); // the denominator: a worker that never ran would satisfy the rest
        TAP_CHECK((seen & iso) == 0);
    }

    void t_isolated_takes_pinned()
    {
        uint32_t const iso = KOS_ISOLATED_CORES;
        TAP_SKIP_UNLESS(iso != 0, "this image isolates no core");
        TAP_ASK(.workers = 1);
        uint32_t const core = pl_lowest(iso);
        int rc = -99;
        pl_sample(0, true, 1u << core, &rc);
        uint32_t const aff = g_pl_aff;
        uint32_t const seen = g_pl_seen;
        tap::diag("pinned to isolated core %u: affinity 0x%x, cores seen 0x%x",
                  static_cast<unsigned>(core), static_cast<unsigned>(aff),
                  static_cast<unsigned>(seen));
        TAP_CHECK(rc == 0);
        TAP_CHECK(aff == (1u << core));
        TAP_CHECK(seen == (1u << core));
    }

    // An isolated core named beside an ordinary one: the mask is admitted whole and verbatim.
    // Which of the two the thread is seen on is the picker's choice and is not asserted.
    void t_isolated_mixed_mask_ok()
    {
        uint32_t const iso = KOS_ISOLATED_CORES;
        TAP_SKIP_UNLESS(iso != 0, "this image isolates no core");
        TAP_ASK(.workers = 1);
        uint32_t const mixed = iso | 1u; // core 0 can never be isolated, so the two are disjoint
        int rc = -99;
        pl_sample(0, true, mixed, &rc);
        uint32_t const aff = g_pl_aff;
        uint32_t const seen = g_pl_seen;
        tap::diag("isolated core named beside core 0: asked 0x%x, affinity 0x%x, cores seen 0x%x",
                  static_cast<unsigned>(mixed), static_cast<unsigned>(aff),
                  static_cast<unsigned>(seen));
        TAP_CHECK(rc == 0);
        TAP_CHECK(aff == mixed);
        TAP_CHECK(seen != 0);
        TAP_CHECK((seen & ~mixed) == 0);
    }

    // --- Preemption: every core's own slice timer takes a thread off it -------------------
    // Each ROUND pins an equal-priority round-robin pair to the core under test and a burner to
    // each other core. Neither of the pair yields or blocks, so while one runs the other's count
    // can move only once that core's own slice expiry has taken the runner off it.
    constexpr unsigned PL_CROWD = KICKOS_KERNEL_CORES + 1u;
    constexpr uint32_t PL_CROWD_JOIN_US = 3u * STALL_TOLERANT_US;

    // How a pair member's spin ended.
    constexpr uint32_t PL_SPENT = 0;    // its budget ran out with no rotation seen
    constexpr uint32_t PL_ROTATED = 1;
    constexpr uint32_t PL_ORPHANED = 2; // the other left first, so nothing could rotate in
    // Quanta a member must have run, unrotated, before a spent budget is the scheduler's.
    constexpr uint32_t PL_SLICE_FLOOR_QUANTA = 8;

    Atomic<uint32_t, Order::RELAXED> g_pl_pass[2];
    Atomic<uint32_t, Order::RELAXED> g_pl_rotated[2];
    Atomic<uint32_t, Order::RELAXED> g_pl_pair_seen[2];
    Atomic<uint32_t, Order::RELAXED> g_pl_pair_done[2];
    // Clock time a member saw itself run: a step of a quantum or more is time off the CPU.
    Atomic<uint32_t, Order::RELAXED> g_pl_pair_ran_us[2];
    uint64_t g_pl_quantum_ns = 0; // published before the round's threads are created

    bool pl_pair_done()
    {
        return g_pl_pair_done[0].load() != 0u and g_pl_pair_done[1].load() != 0u;
    }

    void pl_pair_worker(void* arg)
    {
        unsigned const me = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
        unsigned const other = 1u - me;
        // Released together, so both of the pair are queued before either spins. A spinner runs
        // the instant it is created, and the first could spend its whole bound before main
        // creates the second.
        pl_wait_go();
        uint32_t const from = g_pl_pass[other].load();
        uint64_t const start = kos_clock_now();
        uint64_t last = start;
        uint64_t ran = 0;
        uint32_t pass = 0;
        uint32_t seen = 0;
        uint32_t rotated = PL_SPENT;
        while (true)
        {
            uint64_t const now = kos_clock_now();
            if (now - start >= STALL_TOLERANT_US * 1000ull)
            {
                break;
            }
            if (now - last < g_pl_quantum_ns)
            {
                ran += now - last;
            }
            last = now;
            // Read before the pass: the other publishes its last pass before its done.
            uint32_t const other_done = g_pl_pair_done[other].load();
            if (g_pl_pass[other].load() != from)
            {
                rotated = PL_ROTATED;
            }
            else if (other_done != 0u)
            {
                rotated = PL_ORPHANED;
            }
            // Published after the read, even on the last pass: the other may have taken its
            // own start after this thread's previous pass.
            pass++;
            g_pl_pass[me] = pass;
            if ((pass & 0xFu) == 1u)
            {
                seen |= 1u << pl_core();
            }
            if (rotated != PL_SPENT)
            {
                break;
            }
        }
        g_pl_pair_seen[me] = seen;
        g_pl_rotated[me] = rotated;
        g_pl_pair_ran_us[me] = static_cast<uint32_t>(ran / 1000u);
        g_pl_pair_done[me] = 1;
    }

    void pl_burn_worker(void*)
    {
        pl_wait_go();
        uint64_t const start = kos_clock_now();
        while (not pl_pair_done() and kos_clock_now() - start < 2ull * STALL_TOLERANT_US * 1000ull)
        {
        }
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
        TAP_ASK(.workers = PL_CROWD);
        // Isolated cores are outside the claim. Core 0 can never be isolated, so the target is
        // never empty.
        uint32_t const want = PL_ALL & ~KOS_ISOLATED_CORES;

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
        g_pl_quantum_ns = quantum;
        uint32_t const floor_us =
            static_cast<uint32_t>(PL_SLICE_FLOOR_QUANTA * quantum / 1000u);

        uint32_t unrotated = 0;
        uint32_t starved = 0;
        uint32_t strayed = 0;
        for (uint32_t k = 0; k < static_cast<uint32_t>(KICKOS_KERNEL_CORES); k++)
        {
            if ((want & (1u << k)) == 0u)
            {
                continue;
            }
            for (unsigned i = 0; i < 2u; i++)
            {
                g_pl_pass[i] = 0;
                g_pl_rotated[i] = PL_SPENT;
                g_pl_pair_seen[i] = 0;
                g_pl_pair_done[i] = 0;
                g_pl_pair_ran_us[i] = 0;
            }
            g_pl_go = 0; // released once every thread of the round exists
            kos::thread::Handle w[PL_CROWD];
            ArmHold hold(PL_CROWD_JOIN_US);
            for (unsigned i = 0; i < PL_CROWD; i++)
            {
                TAP_HOLD(hold.thread(&w[i]));
            }
            for (unsigned i = 0; i < PL_CROWD; i++)
            {
                void (*entry)(void*) = pl_pair_worker;
                uint32_t pin = 1u << k;
                if (i >= 2u)
                {
                    entry = pl_burn_worker;
                    pin = 1u << pl_other_core(want, k, i - 2u);
                }
                // The mask goes in at CREATE: pinning afterwards leaves a window in which the
                // pair is runnable anywhere, and one sample there makes it look strayed.
                w[i] = kos::thread::create_caps(entry,
                                                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                                                "burn", 12, nullptr, 0, KOS_POLICY_RR,
                                                static_cast<uint32_t>(quantum), false, nullptr, 0,
                                                0, nullptr, KOS_TASK_NONE, nullptr, 0, pin);
                TAP_CHECK(w[i].valid());
            }
            g_pl_go = 1;
            TAP_CHECK(hold.joined());
            uint32_t const pair_seen = g_pl_pair_seen[0].load() | g_pl_pair_seen[1].load();
            if ((pair_seen & ~(1u << k)) != 0u)
            {
                strayed |= 1u << k;
            }
            for (unsigned i = 0; i < 2u; i++)
            {
                if (g_pl_rotated[i].load() != PL_SPENT)
                {
                    continue;
                }
                if (g_pl_pair_ran_us[i].load() >= floor_us)
                {
                    unrotated |= 1u << k;
                }
                else
                {
                    starved |= 1u << k;
                }
            }
            tap::diag("core %u: the pair ran %u and %u pass(es), %u and %u us, ended %u/%u, "
                      "seen on 0x%x",
                      static_cast<unsigned>(k), static_cast<unsigned>(g_pl_pass[0].load()),
                      static_cast<unsigned>(g_pl_pass[1].load()),
                      static_cast<unsigned>(g_pl_pair_ran_us[0].load()),
                      static_cast<unsigned>(g_pl_pair_ran_us[1].load()),
                      static_cast<unsigned>(g_pl_rotated[0].load()),
                      static_cast<unsigned>(g_pl_rotated[1].load()),
                      static_cast<unsigned>(pair_seen));
        }

        tap::diag("quantum %u ns: wanted 0x%x, unrotated 0x%x, starved 0x%x, strayed 0x%x",
                  static_cast<unsigned>(quantum), static_cast<unsigned>(want),
                  static_cast<unsigned>(unrotated), static_cast<unsigned>(starved),
                  static_cast<unsigned>(strayed));
        // A pair seen off its core means the round staged something else.
        TAP_CHECK(strayed == 0u);
        TAP_CHECK(unrotated == 0u);
        if (starved != 0u)
        {
            TAP_SKIP_VACUOUS("the %u ms budget ran out on cores 0x%x with under %u us run "
                             "unrotated: the host took the time, not the scheduler",
                             static_cast<unsigned>(STALL_TOLERANT_US / 1000u),
                             static_cast<unsigned>(starved), static_cast<unsigned>(floor_us));
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

    // A time budget, sampled until the union is complete: main holds core 0 until it blocks in
    // the join below, so a crowd spending a fixed sample count can finish before core 0 is free
    // and read as never having reached it.
    constexpr uint64_t PL_SPREAD_BUDGET_NS = STALL_TOLERANT_US * 1000ull;

    void pl_spread_worker(void* arg)
    {
        unsigned const me = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
        pl_wait_go();
        g_pl_spread_aff[me] = pl_affinity();
        uint64_t const start = kos_clock_now();
        uint32_t seen = 0;
        uint32_t passes = 0;
        while (true)
        {
            seen |= 1u << pl_core();
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
        TAP_ASK(.workers = PL_CROWD);
        uint32_t const iso = KOS_ISOLATED_CORES;
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
        kos::thread::Handle w[PL_CROWD];
        ArmHold hold(PL_CROWD_JOIN_US);
        for (unsigned i = 0; i < PL_CROWD; i++)
        {
            TAP_HOLD(hold.thread(&w[i]));
        }
        for (unsigned i = 0; i < PL_CROWD; i++)
        {
            w[i] = kos::thread::create(pl_spread_worker,
                                       reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                                       "spread", 12);
            TAP_CHECK(w[i].valid());
        }
        // Last, once every worker exists: released one at a time, the first could finish its
        // samples before anyone could crowd it off a core.
        g_pl_go = 1;
        TAP_CHECK(hold.joined());

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
    constexpr uint8_t XC_SPIN_PRIO = 14;   // above main, below the caller
    constexpr uint8_t XC_CALLER_PRIO = 20; // the thread the reply readies
    constexpr uint8_t XC_SERVER_PRIO = 12;
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
        kos_sem_wait(2, KOS_TIMEOUT_NONE);
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
        g_xc_spin_core = pl_core();
        kos_sem_post(1); // the core is ours; the server may answer now
        while (g_xc_stop.load() == 0)
        {
        }
    }
    void t_resched_reaches_pinned_caller()
    {
        uint32_t const iso = KOS_ISOLATED_CORES;
        uint32_t away = 0;
        for (uint32_t c = 1; c < static_cast<uint32_t>(KICKOS_KERNEL_CORES); c++)
        {
            if ((iso & (1u << c)) == 0)
            {
                away = c;
                break;
            }
        }
        TAP_SKIP_UNLESS(away != 0,
                        "a cross-core wake needs a non-isolated core beside the boot core");
        TAP_ASK(.workers = 3, .sems = 1, .endpoints = 1);
        g_xc_stop = 0;
        g_xc_spin_core = 0xFFFFFFFFu;
        g_xc_call_rc = -99;
        g_xc_serve_rc = -99;
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        kos::thread::Handle sp;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_xc_ep) and hold.cap(&g_xc_gate) and hold.thread(&sv)
                 and hold.thread(&cl) and hold.thread(&sp));
        TAP_CHECK(kos_endpoint_create(&g_xc_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &g_xc_gate) == 0);
        kos_cap_grant vcaps[] = {{g_xc_ep, KOS_CAP_WAIT}, {g_xc_gate, CH_FULL}};
        kos_cap_grant ccaps[] = {{g_xc_ep, KOS_CAP_SIGNAL}};
        kos_cap_grant pcaps[] = {{g_xc_gate, CH_FULL}};
        sv = kos::thread::create_caps(xc_server, nullptr, "xcS", XC_SERVER_PRIO, vcaps, 2,
                                      KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                      KOS_TASK_NONE, nullptr, 0, 1u << PL_HOME);
        TAP_CHECK(sv.valid());
        // The spinner shares the caller's core below it, so it runs only once the caller parks.
        cl = kos::thread::create_caps(xc_caller, nullptr, "xcC", XC_CALLER_PRIO, ccaps, 1,
                                      KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                      KOS_TASK_NONE, nullptr, 0, 1u << away);
        TAP_CHECK(cl.valid());
        sp = kos::thread::create_caps(xc_spinner, nullptr, "xcP", XC_SPIN_PRIO, pcaps, 1,
                                      KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                      KOS_TASK_NONE, nullptr, 0, 1u << away);
        TAP_CHECK(sp.valid());
        // Bounded so a missing reschedule request fails instead of hanging.
        int const cjoined = cl.join(STALL_TOLERANT_US);
        g_xc_stop = 1;
        // Releases the server from its final receive.
        (void)kos_send_timed(g_xc_ep, "", 0, STALL_TOLERANT_US);
        TAP_CHECK(hold.joined());
        uint32_t const spin_core = g_xc_spin_core;
        tap::diag("caller pinned to core %u under a spinner on core %u: join %d, call %d",
                  static_cast<unsigned>(away), static_cast<unsigned>(spin_core), cjoined,
                  static_cast<int>(g_xc_call_rc.load()));
        TAP_CHECK(spin_core == away);
        TAP_CHECK(cjoined == 0);
        TAP_CHECK(g_xc_call_rc.load() == 5);
    }

    // --- A caller that lowers itself takes a thread waiting on a peer core ------------------
    // main raises itself on its core B, then readies W behind an equal-priority spinner pinned
    // to core A, W's mask naming A and B. main's level holds W on A until main lowers itself below
    // W, and the drop that lowering makes must move W to B, where it runs. main spins rather than
    // sleeps throughout: a park would drop B's level by another path.
    constexpr uint8_t PD_HIGH = 20;
    constexpr uint8_t PD_EQUAL = 12;
    constexpr uint8_t PD_LOW = 8;
    constexpr uint64_t PD_SETTLE_NS = 5000000ull;
    Atomic<uint32_t, Order::RELAXED> g_pd_stop{0};
    Atomic<uint32_t, Order::RELAXED> g_pd_spin_core{0xFFFFFFFFu};
    Atomic<uint32_t, Order::RELAXED> g_pd_ran{0};
    Atomic<uint32_t, Order::RELAXED> g_pd_core{0xFFFFFFFFu};

    void pd_spinner(void*)
    {
        g_pd_spin_core = pl_core();
        while (g_pd_stop.load() == 0)
        {
        }
    }

    void pd_waiter(void*)
    {
        g_pd_core = pl_core();
        g_pd_ran = 1;
    }

    // Until `flag` is set or `ns` has passed, on the CPU.
    void pd_spin(Atomic<uint32_t, Order::RELAXED> const* flag, uint64_t ns)
    {
        uint64_t const until = kos_clock_now() + ns;
        while (kos_clock_now() < until)
        {
            if (flag != nullptr and flag->load() != 0)
            {
                return;
            }
        }
    }

    void t_prio_self_lower_moves_waiter()
    {
        TAP_SKIP_UNLESS(g_self->ceiling >= PD_HIGH, "main's ceiling is below %u",
                        static_cast<unsigned>(PD_HIGH));
        TAP_ASK(.workers = 2);
        g_pd_stop = 0;
        g_pd_spin_core = 0xFFFFFFFFu;
        g_pd_ran = 0;
        g_pd_core = 0xFFFFFFFFu;
        kos::thread::Handle x;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&x) and hold.thread(&w));
        // Nothing returns between the raise and the restore.
        int const raised = kos_thread_set_priority(PD_HIGH);
        uint32_t const home = pl_core();
        uint32_t const iso = KOS_ISOLATED_CORES;
        uint32_t away = home;
        for (uint32_t c = 0; c < static_cast<uint32_t>(KICKOS_KERNEL_CORES); c++)
        {
            if (c != home and (iso & (1u << c)) == 0)
            {
                away = c;
                break;
            }
        }
        // FIFO at one level: W is queued behind the spinner on A from its creation.
        if (away != home)
        {
            x = kos::thread::create(pd_spinner, nullptr, "pdX", PD_EQUAL, KOS_POLICY_FIFO, 0,
                                    false, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0,
                                    nullptr, KOS_TASK_NONE, 1u << away);
        }
        if (x.valid())
        {
            w = kos::thread::create(pd_waiter, nullptr, "pdW", PD_EQUAL, KOS_POLICY_FIFO, 0,
                                    false, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0,
                                    nullptr, KOS_TASK_NONE, 1u << away);
        }
        int widened = -99;
        uint32_t early = 0xFFFFFFFFu;
        int lowered = -99;
        uint32_t moved = 0;
        if (w.valid())
        {
            widened = kos_thread_set_affinity(w.id(), (1u << away) | (1u << home));
            // W running before the lowering is a non-event: only a span can stand for it.
            pd_spin(nullptr, PD_SETTLE_NS);
            early = g_pd_ran.load();
            lowered = kos_thread_set_priority(PD_LOW);
            pd_spin(&g_pd_ran, STALL_TOLERANT_US * 1000ull);
            moved = g_pd_ran.load();
        }
        g_pd_stop = 1;
        int const restored = kos_thread_set_priority(g_self->priority);
        TAP_SKIP_UNLESS(away != home, "no non-isolated core beside main's");
        TAP_CHECK(x.valid() and w.valid());
        TAP_CHECK(hold.joined());
        tap::diag("main on core %u lowered with W behind a spinner on core %u: W ran on core %u",
                  static_cast<unsigned>(home), static_cast<unsigned>(away),
                  static_cast<unsigned>(g_pd_core.load()));
        TAP_CHECK(raised == 0 and widened == 0 and lowered == 0 and restored == 0);
        TAP_CHECK(g_pd_spin_core.load() == away);
        TAP_CHECK(early == 0);
        TAP_CHECK(moved == 1);
        TAP_CHECK(g_pd_core.load() == home);
    }

    // --- IRQ delivery across cores ----------------------------------------------------------
    // Every raise of a claimed line is taken on the core that claimed it, whichever core
    // raises it, and a raise one core still holds for a released line reaches no later owner.
    // The owners claim on their own cores, which an ask from main's cannot.
    constexpr int XIRQ_LINE = KICKOS_IRQ_FREE_BASE + 5;
    constexpr int XIRQ_STALE_LINE = KICKOS_IRQ_FREE_BASE + 8;
    constexpr uint32_t XIRQ_CLAIM_CORE = 1;
    constexpr uint32_t XIRQ_OTHER_CORE = 0;
    constexpr uint32_t XIRQ_ALL = 0xFFFFFFFFu;
    constexpr uint32_t XIRQ_QUIET_US = 20000u;
    constexpr uint8_t XIRQ_PRIO = 12;
    constexpr int32_t XIRQ_UNSET = -99;
    // An owner's claim, its wake and its raiser's join, each bounded, in turn.
    constexpr uint32_t XIRQ_JOIN_US = 3u * STALL_TOLERANT_US;

    // Claims `line` and makes the caller the waiter of a fresh notification it signals. 0, or
    // the first refusal.
    int xirq_own(int line, kos_cap_t* irq, kos_cap_t* note)
    {
        int rc = irq_claim_await(line, irq);
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

    kos::thread::Handle xirq_spawn(void (*entry)(void*), void* arg, char const* name, uint32_t core)
    {
        return kos::thread::create_caps(entry, arg, name, XIRQ_PRIO, nullptr, 0, KOS_POLICY_FIFO,
                                        0, false, nullptr, 0, KOS_AUTH_IRQ, nullptr,
                                        KOS_TASK_NONE, nullptr, 0, 1u << core);
    }

    Atomic<int32_t, Order::RELAXED> g_xr_rc{XIRQ_UNSET};
    Atomic<uint32_t, Order::RELAXED> g_xr_core{0xffu};
    Atomic<uint32_t, Order::RELAXED> g_xr_unmade{0};

    void xirq_raiser(void*) // caps: irq(SIGNAL)@1
    {
        g_xr_core = pl_core();
        g_xr_rc = kos_irq_raise(KOS_SPAWN_DELEGATED_CAP0);
    }

    // A thread pinned to `core` that raises `irq` through a SIGNAL copy of it and nothing more.
    kos::thread::Handle xirq_raiser_spawn(kos_cap_t irq, uint32_t core)
    {
        g_xr_rc = XIRQ_UNSET;
        g_xr_core = 0xffu;
        kos_cap_grant const caps[] = {{irq, KOS_CAP_SIGNAL}};
        auto r = kos::thread::create_caps(xirq_raiser, nullptr, "xirqR", XIRQ_PRIO, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, 1u << core);
        if (not r.valid())
        {
            g_xr_unmade = 1;
        }
        return r;
    }

    // The raise's answer once `r` has ended, or XIRQ_UNSET when it was never made or did not end.
    int32_t xirq_raiser_join(kos::thread::Handle const& r)
    {
        if (not r.valid() or r.join(STALL_TOLERANT_US) != 0)
        {
            return XIRQ_UNSET;
        }
        return g_xr_rc.load();
    }

    Atomic<int32_t, Order::RELAXED> g_xw_own{XIRQ_UNSET};
    Atomic<int32_t, Order::RELAXED> g_xw_arm{XIRQ_UNSET};
    Atomic<int32_t, Order::RELAXED> g_xw_first{XIRQ_UNSET};
    Atomic<int32_t, Order::RELAXED> g_xw_second{XIRQ_UNSET};
    Atomic<uint32_t, Order::RELAXED> g_xw_bits{0};
    Atomic<uint32_t, Order::RELAXED> g_xw_core{0xffu};
    Atomic<int32_t, Order::RELAXED> g_xw_raise{XIRQ_UNSET};
    Atomic<uint32_t, Order::RELAXED> g_xw_raise_core{0xffu};

    // Armed before its raiser exists, so the raise lands on an armed line and not in the latch
    // the first arm discards.
    void xirq_waiter(void*)
    {
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        g_xw_own = xirq_own(XIRQ_LINE, &irq, &note);
        if (g_xw_own.load() == 0)
        {
            g_xw_arm = kos_irq_ack(irq);
        }
        if (g_xw_own.load() == 0 and g_xw_arm.load() == 0)
        {
            auto const r = xirq_raiser_spawn(irq, XIRQ_OTHER_CORE);
            if (r.valid())
            {
                uint32_t bits = 0;
                g_xw_first = kos_notify_wait(note, XIRQ_ALL, STALL_TOLERANT_US, &bits);
                g_xw_core = pl_core();
                g_xw_bits = bits;
                bits = 0;
                g_xw_second = kos_notify_wait(note, XIRQ_ALL, XIRQ_QUIET_US, &bits);
                g_xw_raise = xirq_raiser_join(r);
                g_xw_raise_core = g_xr_core.load();
            }
        }
        kos_handle_close(irq);
        kos_handle_close(note);
    }

    void t_irq_cross_core_wake()
    {
        TAP_ASK(.workers = 2, .notifies = 1, .irqs = 1);
        g_xw_own = XIRQ_UNSET;
        g_xw_arm = XIRQ_UNSET;
        g_xw_first = XIRQ_UNSET;
        g_xw_second = XIRQ_UNSET;
        g_xw_bits = 0;
        g_xw_core = 0xffu;
        g_xw_raise = XIRQ_UNSET;
        g_xw_raise_core = 0xffu;
        g_xr_unmade = 0;
        kos::thread::Handle w;
        ArmHold hold(XIRQ_JOIN_US);
        TAP_HOLD(hold.thread(&w));
        w = xirq_spawn(xirq_waiter, nullptr, "xirqW", XIRQ_CLAIM_CORE);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        tap::diag("claimed on core %u, raised from core %u: raise %d, first wait %d bits 0x%x on "
                  "core %u, second wait %d",
                  static_cast<unsigned>(XIRQ_CLAIM_CORE),
                  static_cast<unsigned>(g_xw_raise_core.load()),
                  static_cast<int>(g_xw_raise.load()), static_cast<int>(g_xw_first.load()),
                  static_cast<unsigned>(g_xw_bits.load()), static_cast<unsigned>(g_xw_core.load()),
                  static_cast<int>(g_xw_second.load()));
        TAP_CHECK(g_xr_unmade.load() == 0);
        TAP_CHECK(g_xw_own.load() == 0);
        TAP_CHECK(g_xw_arm.load() == 0);
        TAP_CHECK(g_xw_raise.load() == 0);
        TAP_CHECK(g_xw_raise_core.load() == XIRQ_OTHER_CORE);
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
    Atomic<int32_t, Order::RELAXED> g_xs_raise[XS_LEGS];
    Atomic<int32_t, Order::RELAXED> g_xs_first[XS_LEGS];
    Atomic<int32_t, Order::RELAXED> g_xs_second[XS_LEGS];
    Atomic<uint32_t, Order::RELAXED> g_xs_core[XS_LEGS];

    // Claims and attaches the line and never arms it, so the raise it has made from the other
    // core stays latched when it releases.
    void xirq_stale_owner(void*)
    {
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        g_xs_own[XS_OLD] = xirq_own(XIRQ_STALE_LINE, &irq, &note);
        if (g_xs_own[XS_OLD].load() == 0)
        {
            g_xs_raise[XS_OLD] = xirq_raiser_join(xirq_raiser_spawn(irq, XIRQ_OTHER_CORE));
        }
        kos_handle_close(irq);
        kos_handle_close(note);
    }

    // `arg` is the leg. Its first wait arms the line, and must find no raise: the only one so
    // far was landed for an earlier owner.
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
            auto const r = xirq_raiser_spawn(irq, XIRQ_OTHER_CORE);
            if (r.valid())
            {
                bits = 0;
                g_xs_second[leg] = kos_notify_wait(note, XIRQ_ALL, STALL_TOLERANT_US, &bits);
                g_xs_core[leg] = pl_core();
                g_xs_raise[leg] = xirq_raiser_join(r);
            }
        }
        kos_handle_close(irq);
        kos_handle_close(note);
    }

    // One owner on `core`, run to its end with its raiser made and its line owned.
    bool xirq_leg(void (*entry)(void*), int leg, uint32_t core)
    {
        kos::thread::Handle t;
        ArmHold hold(XIRQ_JOIN_US);
        if (not hold.thread(&t))
        {
            return false;
        }
        t = xirq_spawn(entry, reinterpret_cast<void*>(static_cast<intptr_t>(leg)), "xirqN", core);
        return t.valid() and hold.joined() and g_xr_unmade.load() == 0
               and g_xs_own[leg].load() == 0;
    }

    void t_irq_reclaim_stale_raise()
    {
        TAP_ASK(.workers = 2, .notifies = 1, .irqs = 1);
        for (int leg = 0; leg < XS_LEGS; leg++)
        {
            g_xs_own[leg] = XIRQ_UNSET;
            g_xs_raise[leg] = XIRQ_UNSET;
            g_xs_first[leg] = XIRQ_UNSET;
            g_xs_second[leg] = XIRQ_UNSET;
            g_xs_core[leg] = 0xffu;
        }
        g_xr_unmade = 0;
        TAP_CHECK(xirq_leg(xirq_stale_owner, XS_OLD, XIRQ_CLAIM_CORE));
        TAP_CHECK(xirq_leg(xirq_next_owner, XS_OTHER, XIRQ_OTHER_CORE));
        TAP_CHECK(xirq_leg(xirq_next_owner, XS_BACK, XIRQ_CLAIM_CORE));
        tap::diag("raise %d latched on core %u then released: core %u first wait %d, next raise "
                  "%d delivered %d on core %u; core %u first wait %d, next raise %d delivered %d "
                  "on core %u",
                  static_cast<int>(g_xs_raise[XS_OLD].load()),
                  static_cast<unsigned>(XIRQ_CLAIM_CORE), static_cast<unsigned>(XIRQ_OTHER_CORE),
                  static_cast<int>(g_xs_first[XS_OTHER].load()),
                  static_cast<int>(g_xs_raise[XS_OTHER].load()),
                  static_cast<int>(g_xs_second[XS_OTHER].load()),
                  static_cast<unsigned>(g_xs_core[XS_OTHER].load()),
                  static_cast<unsigned>(XIRQ_CLAIM_CORE),
                  static_cast<int>(g_xs_first[XS_BACK].load()),
                  static_cast<int>(g_xs_raise[XS_BACK].load()),
                  static_cast<int>(g_xs_second[XS_BACK].load()),
                  static_cast<unsigned>(g_xs_core[XS_BACK].load()));
        for (int leg = 0; leg < XS_LEGS; leg++)
        {
            TAP_CHECK(g_xs_raise[leg].load() == 0);
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
    constexpr unsigned RE_WORKERS = RE_SWITCHERS + 1;
    constexpr uint32_t RE_ROUNDS = 2000;
    constexpr uint64_t RE_BUDGET_NS = 2000000000ull; // 2 s
    constexpr uint32_t RE_CHECK_CORE = 1;
    constexpr uint32_t RE_SWITCH_CORE = 0;
    Atomic<uint32_t, Order::RELAXED> g_re_go{0};
    Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> g_re_armed{0};
    Atomic<uint32_t, Order::RELAXED> g_re_rounds[RE_SWITCHERS];
    Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> g_re_done[RE_SWITCHERS];
    Atomic<uint32_t, Order::RELAXED> g_re_bad[RE_WORKERS];
    Atomic<uint32_t, Order::RELAXED> g_re_core[RE_WORKERS];
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
        g_re_core[0] = pl_core();
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
        g_re_core[me + 1] = pl_core();
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
        TAP_ASK(.workers = RE_WORKERS);
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
        for (unsigned i = 0; i < RE_WORKERS; i++)
        {
            g_re_bad[i] = 0;
            g_re_core[i] = 0xffu;
        }
        kos::thread::Handle w[RE_WORKERS];
        ArmHold hold;
        for (unsigned i = 0; i < RE_WORKERS; i++)
        {
            TAP_HOLD(hold.thread(&w[i]));
        }
        int pinned = 0;
        for (unsigned i = 0; i < RE_WORKERS; i++)
        {
            uint32_t core = RE_SWITCH_CORE;
            if (i == 0)
            {
                w[i] = kos::thread::create(re_checker, nullptr, "rechk", 12);
                core = RE_CHECK_CORE;
            }
            else
            {
                w[i] = kos::thread::create(re_switcher,
                                           reinterpret_cast<void*>(static_cast<uintptr_t>(i - 1)),
                                           "reswt", 12);
            }
            TAP_CHECK(w[i].valid());
            pinned |= kos::thread::pin(w[i].id(), core);
        }
        g_re_go = 1;
        TAP_CHECK(hold.joined());
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
        // The precondition: both cores busy with threads of this task for the whole window.
        TAP_CHECK(g_re_core[0].load() == RE_CHECK_CORE);
        for (unsigned i = 1; i < RE_WORKERS; i++)
        {
            TAP_CHECK(g_re_core[i].load() == RE_SWITCH_CORE);
        }
        for (unsigned i = 0; i < RE_WORKERS; i++)
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
    // --- The vector state on every core ------------------------------------------------------
    // EM clear, TS clear and MP set in the machine status word, which SMSW reads from ring 3
    // while CR4.UMIP is clear, and XCR0 naming x87, SSE and AVX, which XGETBV reads there. Both
    // are per core.
    constexpr uint32_t FP_MSW_MASK = (1u << 1) | (1u << 2) | (1u << 3);
    constexpr uint32_t FP_MSW_ENABLED = 1u << 1;
    constexpr uint32_t FP_XCR0 = 7;
    Atomic<uint32_t, Order::RELAXED> g_fp_msw{0};
    Atomic<uint32_t, Order::RELAXED> g_fp_xcr0{0};

    void fp_state_worker(void*)
    {
        pl_wait_go();
        uint16_t msw = 0;
        __asm__ volatile("smsw %0" : "=r"(msw));
        g_fp_msw = msw;
        g_fp_xcr0 = static_cast<uint32_t>(selftest_vec_xcr0());
        g_pl_core = pl_core();
    }

    void t_fp_enabled_every_core()
    {
        TAP_ASK(.workers = 1);
        uint32_t enabled = 0;
        for (uint32_t c = 0; c < static_cast<uint32_t>(KICKOS_KERNEL_CORES); c++)
        {
            pl_reset();
            g_fp_msw = 0;
            g_fp_xcr0 = 0;
            kos::thread::Handle w;
            ArmHold hold;
            TAP_HOLD(hold.thread(&w));
            w = kos::thread::create(fp_state_worker, nullptr, "fpstate", 12);
            TAP_CHECK(w.valid());
            int const rc = kos::thread::pin(w.id(), c);
            g_pl_go = 1;
            bool const joined = hold.joined();
            uint32_t const msw = g_fp_msw;
            uint32_t const xcr0 = g_fp_xcr0;
            uint32_t const core = g_pl_core;
            tap::diag("core %u: machine status word 0x%x, xcr0 0x%x, sampled on core %u",
                      static_cast<unsigned>(c), static_cast<unsigned>(msw),
                      static_cast<unsigned>(xcr0), static_cast<unsigned>(core));
            if (rc == 0 and joined and core == c and (msw & FP_MSW_MASK) == FP_MSW_ENABLED
                and xcr0 == FP_XCR0)
            {
                enabled |= 1u << c;
            }
        }
        TAP_CHECK(enabled == PL_ALL);
    }
#endif
#endif
}
