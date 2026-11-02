// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// budgetwit's peers, one program over every declaration but main's, each running the role its
// argv names, or its instance's name where the init starts it. A role reports to main down the
// endpoint main delegated at KOS_SPAWN_DELEGATED_CAP0, or the server's through /svc/report.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/errno.h>

#include <stdint.h>
#include <string.h>

#include "budgetwit.h"

namespace
{
    constexpr kos_cap_t REPORT = KOS_SPAWN_DELEGATED_CAP0;

    kos_sem_t g_never = KOS_SEM_INIT(0);

    void parked(void*)
    {
        kos_sem_wait(&g_never, KOS_TIMEOUT_NONE);
    }

    template <class R>
    void report(kos_cap_t ep, R const& r)
    {
        (void)kos_send(ep, &r, sizeof(r), KOS_TIMEOUT_NONE);
    }

    // A parked thread of this task, or the spawn's refusal.
    int spawn_parked(uint8_t prio)
    {
        kos_thread_params p{};
        p.entry = parked;
        p.name = "parked";
        p.prio = prio;
        kos_thread_t t = KOS_THREAD_NONE;
        return kos_thread_create(&p, &t);
    }

    // Spawns parked threads until refused, then fills its endpoints until refused, reports both
    // and holds everything until main slays the task.
    int role_hog()
    {
        Report r{};
        while (r.count < SPAWN_CAP)
        {
            int const rc = spawn_parked(PEER_PRIO);
            if (rc != 0)
            {
                r.rc = rc;
                break;
            }
            r.count++;
        }
        kos_cap_t held = KOS_CAP_NONE;
        while (r.extra < SPAWN_CAP)
        {
            int const rc = kos_endpoint_create(&held);
            if (rc != 0)
            {
                r.rc2 = rc;
                break;
            }
            r.extra++;
        }
        report(REPORT, r);
        kos_sem_wait(&g_never, KOS_TIMEOUT_NONE);
        return 0;
    }

    // Spawns `want` parked threads, every one of which must be seated, and reports.
    int role_all(int want)
    {
        Report r{};
        for (int i = 0; i < want; i++)
        {
            int const rc = spawn_parked(PEER_PRIO);
            if (rc != 0)
            {
                r.rc = rc;
                break;
            }
            r.count++;
        }
        report(REPORT, r);
        return 0;
    }

    // The sweeper's children, each above the last: one fills its table with badged copies of
    // the shared endpoint at index 2 and returns. Its sweep closes the only receive capability
    // of the endpoint at index 1 in its first chunk, which wakes the entry parked sending there,
    // so the child stays mid-sweep for as long as the entry or a later child runs above it. The
    // child never receives, so the entry's send lends it no priority. The entry parks nowhere
    // else until it reports, or a paused child would finish its sweep.
    void sweep_child(void* arg)
    {
        uint32_t const badge = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
        kos_cap_t copy = KOS_CAP_NONE;
        while (kos_endpoint_badge(KOS_SPAWN_DELEGATED_CAP0 + 1, badge, &copy) == 0)
        {
        }
    }

    // Children paused in their sweeps one after another until the THREADS budget refuses one.
    int role_sweep()
    {
        Report r{};
        kos_cap_t shared = KOS_CAP_NONE;
        r.rc2 = kos_endpoint_create(&shared);
        while (r.rc2 == 0 and r.count < SWEEP_CAP)
        {
            kos_cap_t wake = KOS_CAP_NONE;
            r.rc2 = kos_endpoint_create(&wake);
            if (r.rc2 != 0)
            {
                break;
            }
            kos_cap_grant const grant[2] = {{wake, KOS_CAP_WAIT},
                                            {shared, KOS_CAP_SIGNAL | KOS_CAP_HANDOUT}};
            kos_thread_params p{};
            p.entry = sweep_child;
            p.arg = reinterpret_cast<void*>(static_cast<uintptr_t>(r.count + 1));
            p.name = "sweep";
            p.prio = static_cast<uint8_t>(SWEEP_CHILD_PRIO + r.count);
            p.caps = grant;
            p.cap_count = 2;
            kos_thread_t t = KOS_THREAD_NONE;
            int const rc = kos_thread_create(&p, &t);
            if (rc != 0)
            {
                r.rc = rc;
                (void)kos_handle_close(wake);
                break;
            }
            // The child's capability is then the endpoint's only receiver.
            uint32_t word = 0;
            if (kos_cap_narrow(wake, KOS_CAP_SIGNAL) == 0)
            {
                r.extra = kos_send(wake, &word, sizeof(word), CALL_US);
            }
            (void)kos_handle_close(wake);
            // Woken by the child's sweep, which stays paused below this entry.
            if (r.extra == -KOS_ECONNREFUSED)
            {
                r.paused++;
            }
            r.count++;
        }
        report(REPORT, r);
        return 0;
    }

    // The free slots of the calling thread's table: a notification and its copies until the table
    // refuses, all closed again; a negative code where something else refused.
    int32_t table_room()
    {
        kos_cap_t n = KOS_CAP_NONE;
        if (kos_notify_create(&n) != 0)
        {
            return -1;
        }
        kos_cap_t copies[WIDTHS_WIDE];
        int32_t c = 0;
        int rc = 0;
        while (rc == 0 and c < static_cast<int32_t>(WIDTHS_WIDE))
        {
            rc = kos_notify_badge(n, static_cast<uint32_t>(c) % 32u, &copies[c]);
            if (rc == 0)
            {
                c++;
            }
        }
        for (int32_t i = 0; i < c; i++)
        {
            (void)kos_handle_close(copies[i]);
        }
        (void)kos_handle_close(n);
        if (rc != -KOS_EMFILE)
        {
            return -2;
        }
        return c + 1;
    }

    // A widths worker: its table's room before it parks and once released.
    struct Worker
    {
        kos_sem_t release = KOS_SEM_INIT(0);
        int32_t seated = 0;
        int32_t kept = 0;
        kos_thread_t thread = KOS_THREAD_NONE;
    };
    // In the task's statics: a thread's stack is its own, and its workers write their counts.
    Worker g_workers[WIDTHS_SMALL];
    Worker g_extra;
    Worker g_wide;
    kos_sem_t g_seated = KOS_SEM_INIT(0);

    void width_worker(void* arg)
    {
        Worker* const w = static_cast<Worker*>(arg);
        w->seated = table_room();
        (void)kos_sem_post(&g_seated);
        (void)kos_sem_wait(&w->release, KOS_TIMEOUT_NONE);
        w->kept = table_room();
    }

    // A worker of `width` slots, its own seated before the call returns; the spawn's code.
    int width_spawn(Worker* w, uint16_t width)
    {
        kos_thread_params p{};
        p.entry = width_worker;
        p.arg = w;
        p.name = "width";
        p.prio = PEER_PRIO - 1;
        p.cap_width = width;
        int const rc = kos_thread_create(&p, &w->thread);
        if (rc == 0)
        {
            (void)kos_sem_wait(&g_seated, CALL_US);
        }
        return rc;
    }

    // Releases `w`, which counts its table again, and waits for its end: its sweep done.
    void width_end(Worker* w)
    {
        (void)kos_sem_post(&w->release);
        (void)kos_thread_join(w->thread, CALL_US);
    }

    // Workers at widths that grow past the declared one and the budget's refusals beside them,
    // then a replacement wider than any of them once two have ended, then the declared width at
    // every slot the budget backs.
    int role_widths()
    {
        WidthsReport r{};
        for (int i = 0; i < 3; i++)
        {
            g_workers[i] = Worker{};
            r.seated[i] = width_spawn(&g_workers[i], WIDTHS_ASK[i]);
            if (r.seated[i] == 0)
            {
                r.seated[i] = g_workers[i].seated;
            }
        }
        g_extra = Worker{};
        r.over = width_spawn(&g_extra, WIDTHS_WIDTH);
        kos_cap_t own = KOS_CAP_NONE;
        (void)kos_notify_create(&own);
        kos_cap_grant const two[2] = {{own, KOS_CAP_SIGNAL}, {own, KOS_CAP_SIGNAL}};
        kos_thread_params p{};
        p.entry = width_worker;
        p.arg = &g_extra;
        p.name = "floor";
        p.prio = PEER_PRIO - 1;
        p.caps = two;
        p.cap_count = 2;
        p.cap_width = KOS_CAP_FIRST_DYNAMIC + 1;
        kos_thread_t floor = KOS_THREAD_NONE;
        r.floor = kos_thread_create(&p, &floor);
        (void)kos_handle_close(own);
        width_end(&g_workers[0]);
        width_end(&g_workers[2]);
        for (int i = 0; i < 3; i++)
        {
            if (i != 1)
            {
                r.kept[i] = g_workers[i].kept;
            }
        }
        g_wide = Worker{};
        r.wide = width_spawn(&g_wide, WIDTHS_WIDE);
        r.wide_seated = g_wide.seated;
        width_end(&g_workers[1]);
        r.kept[1] = g_workers[1].kept;
        if (r.wide == 0)
        {
            width_end(&g_wide);
        }
        while (r.small < WIDTHS_SMALL)
        {
            g_workers[r.small] = Worker{};
            int const rc = width_spawn(&g_workers[r.small], 0);
            if (rc != 0)
            {
                r.small_rc = rc;
                break;
            }
            r.small++;
        }
        if (r.small == WIDTHS_SMALL)
        {
            r.small_rc = width_spawn(&g_extra, 0);
        }
        for (int i = 0; i < r.small; i++)
        {
            width_end(&g_workers[i]);
        }
        report(REPORT, r);
        return 0;
    }

    // One block of `bytes` past the RAM_BYTES budget, then 1-byte blocks until RAM_BLOCKS refuses.
    int role_ram()
    {
        Report r{};
        if (kos_ram_alloc(RAM_BYTES_BUDGET + 1u) == nullptr)
        {
            r.rc = 1;
        }
        while (r.count < SPAWN_CAP and kos_ram_alloc(1) != nullptr)
        {
            r.count++;
        }
        report(REPORT, r);
        return 0;
    }

    // Each start's one block: its status says whether the block was had.
    int role_relaunch()
    {
        if (kos_ram_alloc(RELAUNCH_BLOCK) == nullptr)
        {
            return 1;
        }
        return 0;
    }

    // Each start creates an endpoint of its own and calls main with its code, ending once main
    // replies.
    int role_server()
    {
        Report r{};
        kos_cap_t own = KOS_CAP_NONE;
        r.rc = kos_endpoint_create(&own);
        (void)kos_call(kos_grant_endpoint("/svc/report"), &r, sizeof(r), sizeof(r), KOS_TIMEOUT_NONE);
        return 0;
    }

#if KICKOS_HAVE_ASPACE
    // The pages of the largest run the task can mint now, by halving.
    uint32_t largest_run()
    {
        uint32_t lo = 0;
        uint32_t hi = PAGER_PROBE_PAGES;
        while (lo < hi)
        {
            uint32_t const mid = (lo + hi + 1u) / 2u;
            kos_cap_t run = KOS_CAP_NONE;
            if (kos_frame_create(static_cast<size_t>(mid) * PAGE, &run) == 0)
            {
                (void)kos_handle_close(run);
                lo = mid;
            }
            else
            {
                hi = mid - 1u;
            }
        }
        return lo;
    }

    // The probe, the boundary run, a run mapped and closed, and a run mapped into main's space
    // whose minter then dies, the next incarnation probing again.
    int role_pager(bool after)
    {
        PagesReport r{};
        r.largest = largest_run();
        if (after)
        {
            report(REPORT, r);
            return 0;
        }
        kos_cap_t run = KOS_CAP_NONE;
        r.over = kos_frame_create(static_cast<size_t>(r.largest + 1u) * PAGE, &run);
        kos_cap_t self = KOS_CAP_NONE;
        uint32_t const half = r.largest / 2u;
        if (kos_aspace_self(&self) == 0 and kos_frame_create(half * PAGE, &run) == 0)
        {
            uintptr_t va = 0;
            r.mapped = kos_frame_map(run, self, &va, 0);
            (void)kos_handle_close(run);
            r.after_map = largest_run();
        }
        // Into main's space, delegated at index 1: the mapping outlives this incarnation.
        kos_cap_t const main_space = KOS_SPAWN_DELEGATED_CAP0 + 1;
        if (kos_frame_create(static_cast<size_t>(ELSEWHERE_PAGES) * PAGE, &run) == 0)
        {
            uintptr_t va = 0;
            r.elsewhere = kos_frame_map(run, main_space, &va, 0);
            (void)kos_handle_close(run);
        }
        report(REPORT, r);
        return 0;
    }

    // Two threads reserve at once, each more than half of what the budget leaves.
    kos_sem_t g_race_go = KOS_SEM_INIT(0);
    // A racer keeps its stack, and so its pages, until both have reserved.
    kos_sem_t g_race_leave = KOS_SEM_INIT(0);
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_race_won[2] = {};
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_race_done[2] = {};
    size_t g_race_bytes = 0;

    void racer(void* arg)
    {
        uintptr_t const i = reinterpret_cast<uintptr_t>(arg);
        kos_sem_wait(&g_race_go, KOS_TIMEOUT_NONE);
        if (kos_ram_alloc(g_race_bytes) != nullptr)
        {
            g_race_won[i].store(1u);
        }
        g_race_done[i].store(1u);
        kos_sem_wait(&g_race_leave, KOS_TIMEOUT_NONE);
    }

    int role_race()
    {
        PagesReport r{};
        for (int i = 0; i < 2; i++)
        {
            kos_thread_params p{};
            p.entry = racer;
            p.arg = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
            p.name = "racer";
            p.authority = KOS_AUTH_MEMORY;
            p.prio = static_cast<uint8_t>(PEER_PRIO + i);
            kos_thread_t t = KOS_THREAD_NONE;
            (void)kos_thread_create(&p, &t);
        }
        // Probed once both racers hold their stacks.
        r.largest = largest_run();
        g_race_bytes = static_cast<size_t>(r.largest / 2u + 1u) * PAGE;
        (void)kos_sem_post(&g_race_go);
        (void)kos_sem_post(&g_race_go);
        while (g_race_done[0].load() == 0u or g_race_done[1].load() == 0u)
        {
            kos_sleep_ns(1000000ull);
        }
        r.won = g_race_won[0].load() + g_race_won[1].load();
        (void)kos_sem_post(&g_race_leave);
        (void)kos_sem_post(&g_race_leave);
        report(REPORT, r);
        return 0;
    }
#endif

    bool is(char const* a, char const* b)
    {
        return strcmp(a, b) == 0;
    }
}

int main(int argc, char** argv)
{
    char const* role = argv[0];
    if (argc > 1)
    {
        role = argv[1];
    }
    if (is(role, "hog"))
    {
        return role_hog();
    }
    if (is(role, "all"))
    {
        return role_all(SIBLING_SPAWNS);
    }
    if (is(role, "one"))
    {
        return role_all(1);
    }
    if (is(role, "sweep"))
    {
        return role_sweep();
    }
    if (is(role, "ram"))
    {
        return role_ram();
    }
    if (is(role, "widths"))
    {
        return role_widths();
    }
    if (is(role, "relaunch"))
    {
        return role_relaunch();
    }
    if (is(role, "server"))
    {
        return role_server();
    }
#if KICKOS_HAVE_ASPACE
    if (is(role, "pager"))
    {
        return role_pager(false);
    }
    if (is(role, "pager-after"))
    {
        return role_pager(true);
    }
    if (is(role, "race"))
    {
        return role_race();
    }
#endif
    return 2;
}
