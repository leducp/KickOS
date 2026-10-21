// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The mutex word: where it may live, what the kernel's contended entry takes from it, and the
// waits a removed mapping ends.

#include "selftest.h"

namespace selftest
{
    namespace
    {
        constexpr int32_t WORD_UNSET = -99;
        // How many times a kill staging restages: above one kernel core the kill can land before
        // the park, ending the worker at the wait's entry, which is a staging that did not take.
        constexpr int WORD_STAGE_TRIES = 8;

        uint32_t word_pin()
        {
#if KICKOS_KERNEL_CORES > 1
            return TAP_PIN_CORE;
#else
            return 0;
#endif
        }

        // Ordered against a pinned main by priority alone, in `task` and through `windows`.
        kos::thread::Handle word_spawn(void (*entry)(void*), void* arg, char const* name,
                                       uint8_t prio, kos_cap_grant const* caps, uint8_t count,
                                       kos_task_t task = KOS_TASK_NONE,
                                       kos_window const* windows = nullptr,
                                       uint16_t window_count = 0)
        {
            return kos::thread::create(entry, arg, name, prio, KOS_POLICY_FIFO, 0, nullptr, 0,
                                       nullptr, 0, windows, window_count, caps, count, 0, nullptr,
                                       task, word_pin());
        }

        void word_witness(void*) {}

        // True once every thread above TAP_PRIO_AFTER on main's core has parked or ended: a
        // witness below them and below main runs only then.
        bool word_parked()
        {
            kos::thread::Handle w = word_spawn(word_witness, nullptr, "wordw", TAP_PRIO_AFTER,
                                               nullptr, 0);
            if (not w.valid())
            {
                return false;
            }
            return thread_end(&w) == 0;
        }

#if KICKOS_HAVE_ASPACE
        kos_mutex_t* word_at(uintptr_t va)
        {
            return reinterpret_cast<kos_mutex_t*>(va);
        }
#endif
    }

    // --- A static word contended parks ------------------------------------------------------
    // App statics are writable by every thread of main's task on every board, the sim's host
    // image and a flat board's image included, so the contended entry admits the word.
    kos_mutex_t g_wsp_m = KOS_MUTEX_INIT;
    Atomic<int32_t, Order::RELAXED> g_wsp_rc{WORD_UNSET};
    void wsp_contender(void*)
    {
        int32_t const rc = kos_mutex_lock(&g_wsp_m, STALL_TOLERANT_US);
        g_wsp_rc = rc;
        if (rc == 0)
        {
            (void)kos_mutex_unlock(&g_wsp_m);
        }
    }
    void t_mutex_static_parks()
    {
        TAP_ASK(.workers = 2);
        g_wsp_m.word = 0;
        g_wsp_rc = WORD_UNSET;
        kos::thread::Handle c;
        ArmHold hold;
        TAP_HOLD(hold.owned(&g_wsp_m) and hold.thread(&c));
        TAP_CHECK(kos_mutex_lock(&g_wsp_m, 0) == 0);
        c = word_spawn(wsp_contender, nullptr, "wspC", TAP_PRIO_PARKS, nullptr, 0);
        TAP_CHECK(c.valid() and word_parked());
        int32_t const before = g_wsp_rc;
        TAP_CHECK(kos_mutex_unlock(&g_wsp_m) == 0);
        TAP_CHECK(hold.joined());
        TAP_CHECK(before == WORD_UNSET);
        TAP_CHECK(g_wsp_rc == 0);
        TAP_CHECK(g_wsp_m.word == 0);
    }

    // --- An uncontended lock and unlock is no death point ------------------------------------
    // A thread a kill broke out of its park takes and gives back a free word with no kernel
    // entry, and dies at the syscall after.
    kos_mutex_t g_wkp_m = KOS_MUTEX_INIT;
    Atomic<int32_t, Order::RELAXED> g_wkp_wait{WORD_UNSET};
    Atomic<int32_t, Order::RELAXED> g_wkp_locked{WORD_UNSET};
    Atomic<int32_t, Order::RELAXED> g_wkp_unlocked{WORD_UNSET};
    void wkp_worker(void*) // caps: done@0, park@1. Nothing posts the park.
    {
        kos_sem_post(CH_DONE);
        g_wkp_wait = kos_sem_wait(child_cap(1), KOS_TIMEOUT_NONE);
        g_wkp_locked = kos_mutex_lock(&g_wkp_m, KOS_TIMEOUT_NONE);
        g_wkp_unlocked = kos_mutex_unlock(&g_wkp_m);
        kos_exit(0); // the death point
    }
    void t_mutex_kill_pending()
    {
        TAP_ASK(.workers = 1, .sems = 1);
        kos_cap_t park = KOS_CAP_NONE;
        kos::thread::Handle killed;
        ArmHold hold;
        TAP_HOLD(hold.cap(&park) and hold.thread(&killed));
        TAP_CHECK(kos_sem_create(0, &park) == 0);
        g_wkp_m.word = 0;
        g_wkp_wait = WORD_UNSET;
        for (int attempt = 0; attempt < WORD_STAGE_TRIES and g_wkp_wait != -KOS_ECANCELED;
             attempt++)
        {
            g_wkp_locked = WORD_UNSET;
            g_wkp_unlocked = WORD_UNSET;
            kos_cap_grant const caps[2] = {{g_done, CH_FULL}, {park, CH_FULL}};
            killed = kos::thread::create_caps(wkp_worker, nullptr, "wkpk", 10, caps, 2);
            TAP_CHECK(killed.valid());
            TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
            TAP_CHECK(killed.kill() == 0);
            TAP_CHECK(thread_end(&killed) == 0);
        }
        if (g_wkp_wait != -KOS_ECANCELED)
        {
            TAP_SKIP_VACUOUS("the park was never reached in %d stagings", WORD_STAGE_TRIES);
            return;
        }
        TAP_CHECK(g_wkp_locked == 0);
        TAP_CHECK(g_wkp_unlocked == 0);
        TAP_CHECK(g_wkp_m.word == 0);
        TAP_CHECK(hold.close(&park) == 0);
    }

    // --- A holder slain holding a word nobody waited on -------------------------------------
    // Its id stays in the word. The next take answers the death once, and a thread now holding
    // the dead holder's slot, at a new generation, does not stand in for it.
    kos_mutex_t g_wsh_m = KOS_MUTEX_INIT;
    kos::Mutex g_wsh_wrapped;
    void wsh_holder(void*) // caps: done@0, park@1
    {
        (void)kos_mutex_lock(&g_wsh_m, KOS_TIMEOUT_NONE);
        (void)g_wsh_wrapped.lock();
        kos_sem_post(CH_DONE);
        (void)kos_sem_wait(child_cap(1), KOS_TIMEOUT_NONE);
    }
    void wsh_reuser(void*) // caps: park@0
    {
        (void)kos_sem_wait(child_cap(0), KOS_TIMEOUT_NONE);
    }
    void t_mutex_slain_holder()
    {
        TAP_ASK(.workers = 2, .sems = 1);
        kos_cap_t park = KOS_CAP_NONE;
        kos::thread::Handle holder;
        kos::thread::Handle reuser;
        g_wsh_m.word = 0;
        ArmHold hold;
        TAP_HOLD(hold.owned(&g_wsh_m) and hold.cap(&park) and hold.thread(&holder)
                 and hold.thread(&reuser));
        TAP_CHECK(kos_sem_create(0, &park) == 0);
        kos_cap_grant const hcaps[2] = {{g_done, CH_FULL}, {park, CH_FULL}};
        holder = kos::thread::create_caps(wsh_holder, nullptr, "wshH", 10, hcaps, 2);
        TAP_CHECK(holder.valid());
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        kos_thread_t const dead = holder.id();
        TAP_CHECK(holder.slay(STALL_TOLERANT_US) == 0);
        holder = kos::thread::Handle();
        // The lowest exited slot is the one the slay just emptied.
        kos_cap_grant const rcaps[1] = {{park, CH_FULL}};
        reuser = kos::thread::create_caps(wsh_reuser, nullptr, "wshR", 10, rcaps, 1);
        TAP_CHECK(reuser.valid());
        bool const reused = (reuser.id() & 0xFFFFu) == (dead & 0xFFFFu);
        int const first = kos_mutex_lock(&g_wsh_m, 0);
        int const unlocked = kos_mutex_unlock(&g_wsh_m);
        int const second = kos_mutex_lock(&g_wsh_m, 0);
        int const released = kos_mutex_unlock(&g_wsh_m);
        // A try that steals the dead holder's lock holds it.
        bool const wrapped = g_wsh_wrapped.try_lock();
        int wrapped_unlocked = -1;
        if (wrapped)
        {
            wrapped_unlocked = g_wsh_wrapped.unlock();
        }
        kos_sem_post(park);
        TAP_CHECK(hold.joined());
        tap::diag("slain holder: slot reused %d, first take %d, second %d",
                  static_cast<int>(reused), first, second);
        TAP_CHECK(first == -KOS_EOWNERDEAD);
        TAP_CHECK(unlocked == 0);
        TAP_CHECK(second == 0);
        TAP_CHECK(released == 0);
        TAP_CHECK(g_wsh_m.word == 0);
        TAP_CHECK(wrapped and wrapped_unlocked == 0);
        TAP_SKIP_UNLESS(reused, "the slain holder's slot went to no new thread");
    }

#if KICKOS_MEMORY_ENFORCED
    // --- A forged holder in memory only main reaches -----------------------------------------
    // The word names a live thread of another task, which cannot write main's block: the
    // kernel takes the id for a dead holder's, and boosts nobody.
    constexpr uint32_t WFO_WAIT_US = 500000;
    void wfo_victim(void*) // caps: E(SIGNAL)@0, park@1
    {
        uint32_t const id = kickos_self_lock_id();
        (void)kos_send(child_cap(0), &id, sizeof(id));
        (void)kos_sem_wait(child_cap(1), KOS_TIMEOUT_NONE);
    }
    void t_mutex_forged_foreign()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .sems = 1, .endpoints = 1);
        void* const blk = st_ram<WFO_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        TAP_CHECK(kos_mem_self_grant(blk, 64, 0) == 0);
        kos_mutex_t* const m = static_cast<kos_mutex_t*>(blk);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t park = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle v;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.cap(&park) and hold.task(&t) and hold.thread(&v));
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_sem_create(0, &park) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        kos_cap_grant const caps[2] = {{ep, KOS_CAP_SIGNAL}, {park, KOS_CAP_WAIT}};
        v = word_spawn(wfo_victim, nullptr, "wfoV", TAP_PRIO_PARKS, caps, 2, t);
        TAP_CHECK(v.valid());
        uint32_t id = 0;
        TAP_CHECK(report_await(ep, &id, sizeof(id)));
        TAP_CHECK(id != 0);
        m->word = id;
        uint64_t const t0 = kos_clock_now();
        int const rc = kos_mutex_lock(m, WFO_WAIT_US);
        uint64_t const took_us = (kos_clock_now() - t0) / 1000u;
        int const unlocked = kos_mutex_unlock(m);
        kos_sem_post(park);
        TAP_CHECK(rc == -KOS_EOWNERDEAD);
        // No park, so no boost: the answer came before the deadline it would have waited out.
        TAP_CHECK(took_us < WFO_WAIT_US);
        TAP_CHECK(unlocked == 0 and m->word == 0);
    }
#endif

#if KICKOS_HAVE_ASPACE
    // --- A word on a worker's stack, shared with its siblings ---------------------------------
    // A stack the kernel allocated is mapped task-wide and never granted. Its owner holds a word
    // on it while main waits, then waits on it while main holds it.
    kos_mutex_t* volatile g_wst_word = nullptr;
    Atomic<int32_t, Order::RELAXED> g_wst_main_in{0};
    Atomic<int32_t, Order::RELAXED> g_wst_released{0};
    Atomic<int32_t, Order::RELAXED> g_wst_rc{WORD_UNSET};
    constexpr uint64_t WST_POLL_NS = 1000000ull;
    void wst_owner(void*)
    {
        kos_mutex_t on_stack = KOS_MUTEX_INIT;
        if (kos_mutex_lock(&on_stack, 0) != 0)
        {
            return;
        }
        g_wst_word = &on_stack;
        while (g_wst_main_in == 0)
        {
            kos_sleep_ns(WST_POLL_NS);
        }
        // main sets the flag just before it asks, so it is waiting by now.
        kos_sleep_ns(4u * WST_POLL_NS);
        g_wst_released = 1;
        (void)kos_mutex_unlock(&on_stack);
        // Handed to main, which runs below this thread: this waits on main.
        int32_t const rc = kos_mutex_lock(&on_stack, STALL_TOLERANT_US);
        g_wst_rc = rc;
        if (rc == 0)
        {
            (void)kos_mutex_unlock(&on_stack);
        }
        g_wst_word = nullptr;
    }
    void t_mutex_stack_word()
    {
        TAP_ASK(.workers = 3);
        g_wst_word = nullptr;
        g_wst_main_in = 0;
        g_wst_released = 0;
        g_wst_rc = WORD_UNSET;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        w = word_spawn(wst_owner, nullptr, "wstO", TAP_PRIO_PARKS, nullptr, 0);
        TAP_CHECK(w.valid() and word_parked());
        kos_mutex_t* const m = g_wst_word;
        TAP_CHECK(m != nullptr);
        g_wst_main_in = 1;
        int const took = kos_mutex_lock(m, STALL_TOLERANT_US);
        int32_t const released = g_wst_released;
        bool const owner_waits = word_parked();
        int32_t const before = g_wst_rc;
        int const gave = kos_mutex_unlock(m);
        TAP_CHECK(hold.joined());
        TAP_CHECK(took == 0 and released == 1); // main waited on the owner's stack word
        TAP_CHECK(owner_waits and before == WORD_UNSET and gave == 0);
        TAP_CHECK(g_wst_rc == 0); // and the owner waited on main
    }

    // What a thread of another task saw of a word it reached through a memory window.
    struct WordRep
    {
        int32_t rc;
        uint32_t id;
        uintptr_t base;
    };
    // Reaches the word at its first window's base and reports, unlocking what it took.
    void word_window_take(kos_cap_t ep, uint32_t timeout_us, uint32_t forge)
    {
        WordRep rep = {WORD_UNSET, kickos_self_lock_id(), 0};
        kos_window at = {};
        if (kos_window_get(0, &at) == 0)
        {
            rep.base = at.base;
            kos_mutex_t* const m = word_at(at.base);
            if (forge != 0)
            {
                m->word = forge;
            }
            rep.rc = kos_mutex_lock(m, timeout_us);
            if (rep.rc == 0 or rep.rc == -KOS_EOWNERDEAD)
            {
                (void)kos_mutex_unlock(m);
            }
        }
        (void)kos_send(ep, &rep, sizeof(rep));
    }

    // --- One word through a parent's reservation and a child's window ------------------------
    // The window sits at a kernel-chosen address in the child's space, and the two addresses
    // name one word: the child waits on the parent and is handed it.
    void wwi_child(void*) // caps: E(SIGNAL)@0
    {
        word_window_take(child_cap(0), STALL_TOLERANT_US, 0);
    }
    void t_mutex_window_shared()
    {
        TAP_ASK(.workers = 2, .tasks = 1, .endpoints = 1);
        size_t const g = arena_granule();
        void* const blk = st_ram<WWI_RAM, 0>();
        TAP_CHECK(blk != nullptr and g != 0);
        TAP_CHECK(kos_mem_self_grant(blk, g, 0) == 0);
        kos_mutex_t* const m = static_cast<kos_mutex_t*>(blk);
        m->word = 0;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle c;
        ArmHold hold;
        TAP_HOLD(hold.owned(m) and hold.cap(&ep) and hold.task(&t) and hold.thread(&c));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        TAP_CHECK(kos_mutex_lock(m, 0) == 0);
        kos_window const w = {reinterpret_cast<uintptr_t>(blk), static_cast<uint32_t>(g),
                              KOS_WINDOW_MEMORY, 0};
        kos_cap_grant const caps[1] = {{ep, KOS_CAP_SIGNAL}};
        c = word_spawn(wwi_child, nullptr, "wwiC", TAP_PRIO_PARKS, caps, 1, t, &w, 1);
        TAP_CHECK(c.valid() and word_parked());
        // The child is parked on the word, so this unlock hands it over.
        int const unlocked = kos_mutex_unlock(m);
        WordRep rep = {WORD_UNSET, 0, 0};
        TAP_CHECK(report_await(ep, &rep, sizeof(rep)));
        TAP_CHECK(hold.joined());
        tap::diag("window word: main 0x%lx, child 0x%lx, child's take %ld",
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(blk)),
                  static_cast<unsigned long>(rep.base), static_cast<long>(rep.rc));
        TAP_CHECK(rep.base != 0 and rep.base != reinterpret_cast<uintptr_t>(blk));
        TAP_CHECK(unlocked == 0);
        TAP_CHECK(rep.rc == 0);
        TAP_CHECK(m->word == 0);
    }

    // --- One frame run mapped at two addresses, a waiter through each ------------------------
    constexpr uint32_t WFA_WAIT_US = 1000000;
    uintptr_t g_wfa_va[2] = {0, 0};
    Atomic<int32_t, Order::RELAXED> g_wfa_rc0{WORD_UNSET};
    Atomic<int32_t, Order::RELAXED> g_wfa_rc1{WORD_UNSET};
    void wfa_waiter(void* arg)
    {
        uintptr_t const i = reinterpret_cast<uintptr_t>(arg);
        kos_mutex_t* const m = word_at(g_wfa_va[i]);
        int32_t const rc = kos_mutex_lock(m, WFA_WAIT_US);
        if (rc == 0)
        {
            (void)kos_mutex_unlock(m);
        }
        if (i == 0)
        {
            g_wfa_rc0 = rc;
        }
        else
        {
            g_wfa_rc1 = rc;
        }
    }
    void t_mutex_frame_two_addresses()
    {
        TAP_ASK(.workers = 3);
        size_t const g = ASPACE_GRANULE;
        kos_cap_t frame = KOS_CAP_NONE;
        kos_cap_t space = KOS_CAP_NONE;
        kos_cap_t spot = KOS_CAP_NONE;
        uintptr_t va = 0;
        kos::thread::Handle w0;
        kos::thread::Handle w1;
        g_wfa_rc0 = WORD_UNSET;
        g_wfa_rc1 = WORD_UNSET;
        ArmHold hold;
        TAP_HOLD(hold.mapped(&frame, &space, &va) and hold.cap(&spot) and hold.thread(&w0)
                 and hold.thread(&w1));
        TAP_CHECK(kos_frame_create(g, &frame) == 0 and kos_aspace_self(&space) == 0);
        // A second run held unmapped keeps its own address free for the second mapping.
        TAP_CHECK(kos_frame_create(g, &spot) == 0);
        uintptr_t second = 0;
        TAP_CHECK(kos_frame_map(spot, space, &second, 0) == 0
                  and kos_frame_unmap(spot, space, second) == 0);
        TAP_CHECK(kos_frame_map(frame, space, &va, 0) == 0 and va != 0);
        TAP_CHECK(map_at(frame, space, second, 0) == 0);
        g_wfa_va[0] = va;
        g_wfa_va[1] = second;
        kos_mutex_t* const m = word_at(va);
        m->word = 0;
        int const taken = kos_mutex_lock(m, 0);
        w0 = word_spawn(wfa_waiter, reinterpret_cast<void*>(uintptr_t{0}), "wfa0",
                        TAP_PRIO_PARKS, nullptr, 0);
        w1 = word_spawn(wfa_waiter, reinterpret_cast<void*>(uintptr_t{1}), "wfa1",
                        TAP_PRIO_PARKS, nullptr, 0);
        bool const parked = word_parked();
        int const unlocked = kos_mutex_unlock(m);
        bool const joined = hold.joined();
        uint32_t const left = m->word;
        int const unmapped = kos_frame_unmap(frame, space, second);
        TAP_CHECK(taken == 0 and w0.valid() and w1.valid() and parked and unlocked == 0
                  and joined);
        TAP_CHECK(unmapped == 0);
        // Each served in turn: one record for the one word, whichever address named it.
        TAP_CHECK(g_wfa_rc0 == 0);
        TAP_CHECK(g_wfa_rc1 == 0);
        TAP_CHECK(left == 0);
    }

    // --- Two tasks whose windows land at one address -----------------------------------------
    // A victim task holds its word with a waiter parked; a forger task's window over another
    // block lands at the same address and names the victim's holder. The forger's frame is not
    // the victim's, so it takes its own word as a dead holder's, and the victim's waiter is
    // still the one served.
    constexpr uint32_t WTW_WAIT_US = 1000000;
    void wtw_holder(void*) // caps: E(SIGNAL)@0, release@1
    {
        WordRep rep = {WORD_UNSET, kickos_self_lock_id(), 0};
        kos_window at = {};
        if (kos_window_get(0, &at) == 0)
        {
            rep.base = at.base;
            rep.rc = kos_mutex_lock(word_at(at.base), 0);
        }
        (void)kos_send(child_cap(0), &rep, sizeof(rep));
        (void)kos_sem_wait(child_cap(1), KOS_TIMEOUT_NONE);
        if (rep.rc == 0)
        {
            (void)kos_mutex_unlock(word_at(at.base));
        }
        // The task's entry: its end would slay the waiter before that reports.
        (void)kos_sem_wait(child_cap(1), KOS_TIMEOUT_NONE);
    }
    void wtw_waiter(void*) // caps: E(SIGNAL)@0
    {
        word_window_take(child_cap(0), WTW_WAIT_US, 0);
    }
    void wtw_forger(void* arg) // caps: E(SIGNAL)@0
    {
        word_window_take(child_cap(0), WTW_WAIT_US,
                         static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg)));
    }
    void t_mutex_window_forged()
    {
        TAP_ASK(.workers = 4, .tasks = 2, .sems = 1, .endpoints = 1);
        size_t const g = arena_granule();
        void* const vblk = st_ram<WTW_RAM, 0>();
        void* const fblk = st_ram<WTW_RAM, 1>();
        TAP_CHECK(vblk != nullptr and fblk != nullptr and g != 0);
        TAP_CHECK(kos_mem_self_grant(vblk, g, 0) == 0 and kos_mem_self_grant(fblk, g, 0) == 0);
        static_cast<kos_mutex_t*>(vblk)->word = 0;
        static_cast<kos_mutex_t*>(fblk)->word = 0;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t release = KOS_CAP_NONE;
        kos_task_t victim = KOS_TASK_NONE;
        kos_task_t forger = KOS_TASK_NONE;
        kos::thread::Handle vh;
        kos::thread::Handle vw;
        kos::thread::Handle fh;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.cap(&release) and hold.task(&victim)
                 and hold.task(&forger) and hold.thread(&vh) and hold.thread(&vw)
                 and hold.thread(&fh));
        TAP_CHECK(kos_endpoint_create(&ep) == 0 and kos_sem_create(0, &release) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &victim) == 0
                  and kos_task_create(nullptr, 0, 0, &forger) == 0);
        kos_window const vwin = {reinterpret_cast<uintptr_t>(vblk), static_cast<uint32_t>(g),
                                 KOS_WINDOW_MEMORY, 0};
        kos_window const fwin = {reinterpret_cast<uintptr_t>(fblk), static_cast<uint32_t>(g),
                                 KOS_WINDOW_MEMORY, 0};
        kos_cap_grant const hcaps[2] = {{ep, KOS_CAP_SIGNAL}, {release, KOS_CAP_WAIT}};
        kos_cap_grant const caps[1] = {{ep, KOS_CAP_SIGNAL}};
        vh = word_spawn(wtw_holder, nullptr, "wtwH", TAP_PRIO_PARKS, hcaps, 2, victim, &vwin, 1);
        TAP_CHECK(vh.valid());
        WordRep held = {WORD_UNSET, 0, 0};
        TAP_CHECK(report_await(ep, &held, sizeof(held)));
        TAP_CHECK(held.rc == 0 and held.id != 0);
        // Parks on the victim's word before main resumes.
        vw = word_spawn(wtw_waiter, nullptr, "wtwW", TAP_PRIO_PARKS, caps, 1, victim, &vwin, 1);
        TAP_CHECK(vw.valid() and word_parked());
        // Above the victim's waiter, so a record it shared would hand the victim's word to it.
        fh = word_spawn(wtw_forger, reinterpret_cast<void*>(static_cast<uintptr_t>(held.id)),
                        "wtwF", TAP_PRIO_PARKS + 1, caps, 1, forger, &fwin, 1);
        TAP_CHECK(fh.valid());
        WordRep forged = {WORD_UNSET, 0, 0};
        bool const heard_forged = report_await(ep, &forged, sizeof(forged));
        kos_sem_post(release);
        WordRep served = {WORD_UNSET, 0, 0};
        bool const heard_served = report_await(ep, &served, sizeof(served));
        kos_sem_post(release);
        TAP_CHECK(hold.joined());
        tap::diag("windows: victim 0x%lx, forger 0x%lx; forger %ld, victim's waiter %ld",
                  static_cast<unsigned long>(held.base), static_cast<unsigned long>(forged.base),
                  static_cast<long>(forged.rc), static_cast<long>(served.rc));
        TAP_CHECK(heard_forged and heard_served);
        TAP_SKIP_UNLESS(forged.base == held.base,
                        "the two tasks' windows did not land at one address");
        TAP_CHECK(forged.rc == -KOS_EOWNERDEAD);
        TAP_CHECK(served.rc == 0);
        TAP_CHECK(static_cast<kos_mutex_t*>(vblk)->word == 0);
    }

    // --- A removed mapping ends the waits through it -----------------------------------------
    constexpr uint32_t WRL_WAIT_US = 1000000;
    Atomic<int32_t, Order::RELAXED> g_wrl_rc{WORD_UNSET};
    void wrl_waiter(void* arg)
    {
        g_wrl_rc = kos_mutex_lock(static_cast<kos_mutex_t*>(arg), WRL_WAIT_US);
    }
    // A frame capability's mapping unmapped by a sibling of the waiter.
    void t_mutex_unmap_ends_wait()
    {
        TAP_ASK(.workers = 2);
        kos_cap_t frame = KOS_CAP_NONE;
        kos_cap_t space = KOS_CAP_NONE;
        uintptr_t va = 0;
        kos::thread::Handle w;
        g_wrl_rc = WORD_UNSET;
        ArmHold hold;
        TAP_HOLD(hold.mapped(&frame, &space, &va) and hold.thread(&w));
        TAP_CHECK(kos_frame_create(ASPACE_GRANULE, &frame) == 0 and kos_aspace_self(&space) == 0);
        TAP_CHECK(kos_frame_map(frame, space, &va, 0) == 0 and va != 0);
        kos_mutex_t* const m = word_at(va);
        m->word = 0;
        TAP_CHECK(kos_mutex_lock(m, 0) == 0);
        w = word_spawn(wrl_waiter, m, "wrlU", TAP_PRIO_PARKS, nullptr, 0);
        TAP_CHECK(w.valid() and word_parked());
        int32_t const parked = g_wrl_rc;
        int const unmapped = kos_frame_unmap(frame, space, va);
        if (unmapped == 0)
        {
            va = 0;
        }
        TAP_CHECK(hold.joined());
        TAP_CHECK(parked == WORD_UNSET and unmapped == 0);
        TAP_CHECK(g_wrl_rc == -KOS_EFAULT);
    }

    // A reservation retyped uncached by a sibling of the waiter: a self-grant cannot narrow the
    // rights, so the memory type is what it takes.
    void t_mutex_retype_ends_wait()
    {
        TAP_ASK(.workers = 2);
        size_t const g = arena_granule();
        void* const blk = st_ram<WRT_RAM, 0>();
        TAP_CHECK(blk != nullptr and g != 0);
        TAP_CHECK(kos_mem_self_grant(blk, g, 0) == 0);
        kos_mutex_t* const m = static_cast<kos_mutex_t*>(blk);
        m->word = 0;
        kos::thread::Handle w;
        g_wrl_rc = WORD_UNSET;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        TAP_CHECK(kos_mutex_lock(m, 0) == 0);
        w = word_spawn(wrl_waiter, m, "wrlR", TAP_PRIO_PARKS, nullptr, 0);
        TAP_CHECK(w.valid() and word_parked());
        int32_t const parked = g_wrl_rc;
        int const retyped = kos_mem_self_grant(blk, g, KOS_MEM_NOCACHE);
        bool const joined = hold.joined();
        int const restored = kos_mem_self_grant(blk, g, 0);
        (void)kos_mutex_unlock(m);
        TAP_SKIP_UNLESS(retyped != -KOS_ENOTSUP, "this chip maps no uncached memory");
        TAP_CHECK(joined and parked == WORD_UNSET);
        TAP_CHECK(retyped == 0 and restored == 0);
        TAP_CHECK(g_wrl_rc == -KOS_EFAULT);
    }
#endif
}
