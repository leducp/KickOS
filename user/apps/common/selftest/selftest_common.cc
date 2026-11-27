// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The definitions every self-test translation unit reaches.

// The one translation unit that reserves: st_ram_seat.
#define KICKOS_SELFTEST_RAM_SEAT 1
#include "selftest.h"
#include <unistd.h>

#include <kickos/apps/peer_run.h>
#include <kickos/config/priorities.h> // KICKOS_PRIO_MIN

namespace selftest
{
    kos_sem_t g_done = KOS_SEM_INIT(0);
    kos_sem_t g_lock = KOS_SEM_INIT(1);
    kos_self_t const* g_self = nullptr;
    kos_thread_t g_main = KOS_THREAD_NONE;

    bool report_await(kos_cap_t ep, void* rep, size_t len)
    {
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, KOS_RECV_NO_INFO, after_us(STALL_TOLERANT_US));
        return kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, len), &opts)
               == static_cast<int>(len);
    }

    namespace
    {
        // Slays *h and joins it within ARM_SLAY_US: 0 once it is gone, -KOS_EBADF for one already
        // gone, else the cancel's or the join's answer.
        int thread_slay_gone(kos::thread::Handle const* h)
        {
            int const slain = h->cancel(KOS_CANCEL_SLAY);
            if (slain != 0)
            {
                return slain;
            }
            return h->join_for(std::chrono::microseconds{ARM_SLAY_US});
        }
    }

    int thread_end(kos::thread::Handle* h, uint32_t bound_us)
    {
        int const rc = h->join_for(std::chrono::microseconds{bound_us});
        if (rc != 0)
        {
            int const slain = thread_slay_gone(h);
            if (slain != 0 and slain != -KOS_EBADF)
            {
                tap::fail("thread %u: join answered %d, slay %d", static_cast<unsigned>(h->id()),
                          rc, slain);
                return rc;
            }
        }
        *h = kos::thread::Handle();
        return rc;
    }

    int slay_dead(kos_task_t t, kos_monotonic_t until, kos_cap_t note)
    {
        if (note == KOS_CAP_NONE)
        {
            return kickos::apps::slay_until_dead(t, until);
        }
        kos_cap_t badged = KOS_CAP_NONE;
        int const rc = kos_notify_badge(note, PEER_START_BIT, &badged);
        if (rc != 0)
        {
            (void)kos_task_slay(t);
            return rc;
        }
        int const dead = kickos::apps::slay_until_dead(t, until, badged, PEER_START_BIT);
        (void)kos_handle_close(badged);
        return dead;
    }

    int task_end(kos_task_t* t, uint32_t bound_us, kos_cap_t note)
    {
        int const rc = slay_dead(*t, after_us(bound_us), note);
        if (rc != 0)
        {
            tap::fail("task %u: slay answered %d", static_cast<unsigned>(*t), rc);
        }
        *t = KOS_TASK_NONE;
        return rc;
    }

    namespace
    {
        // A sleep and not a yield: a poller above the thread it waits on would hold its core.
        constexpr int64_t FLAG_STEP_NS = 500000;

        bool flag_reaches(Atomic<uint32_t, Order::RELAXED> const& cell, uint32_t value,
                          bool equal)
        {
            kos_monotonic_t const deadline = after_us(STALL_TOLERANT_US);
            while ((cell.load() == value) != equal)
            {
                if (kos_monotonic_now().ns > deadline.ns)
                {
                    return false;
                }
                kos_sleep_for(kos_ns(FLAG_STEP_NS));
            }
            return true;
        }
    }

    bool flag_await(Atomic<uint32_t, Order::RELAXED> const& cell, uint32_t want)
    {
        return flag_reaches(cell, want, true);
    }

    bool flag_await_change(Atomic<uint32_t, Order::RELAXED> const& cell, uint32_t from)
    {
        return flag_reaches(cell, from, false);
    }

    bool others_parked()
    {
        if (kos_thread_set_priority(KICKOS_PRIO_MIN) != 0)
        {
            return false;
        }
        kos_yield();
        return kos_thread_set_priority(g_self->priority) == 0;
    }

    bool main_above(uint8_t prio)
    {
        return kos_thread_set_priority(prio) == 0;
    }

    bool main_back()
    {
        return kos_thread_set_priority(g_self->priority) == 0;
    }

    // Above every batch an arm asks for: a crowd of one per kernel core plus one, or four.
    constexpr int POOL_PROBES_MAX = KICKOS_KERNEL_CORES + 4;

    namespace
    {
        kos_sem_t g_probe_gate = KOS_SEM_INIT(0);
    }

    void pool_gated_worker(void*)
    {
        kos_sem_wait(&g_probe_gate, KOS_NEVER);
    }

    namespace
    {
        // The first supply an ask ran out of: `got` of the `want` it states.
        struct Refusal
        {
            int rc = 0;
            char const* pool = nullptr;
            int got = 0;
            int want = 0;
        };

        void note(Refusal* r, int rc, char const* pool, int got, int want)
        {
            if (r->rc == 0 and rc != 0)
            {
                r->rc = rc;
                r->pool = pool;
                r->got = got;
                r->want = want;
            }
        }

        // Can this board host `n` workers CONCURRENTLY, right now? Slots held by driver tasks and
        // room for each stack bound this as much as the thread pool's size does. No probe leaves
        // before the whole batch exists, and each is joined, so on return every probe slot and
        // stack is free again.
        void workers_refusal(int n, Refusal* r)
        {
            int rc = kos_sem_init(&g_probe_gate, 0);
            kos::thread::Handle probes[POOL_PROBES_MAX];
            int got = 0;
            for (int i = 0; i < n; i++)
            {
                probes[i] =
                    kos::thread::create_caps(pool_gated_worker, nullptr, "probe", 10, nullptr, 0);
                if (not probes[i].valid())
                {
                    rc = probes[i].error();
                    break;
                }
                got++;
            }
            note(r, rc, "workers", got, n);
            for (int i = 0; i < got; i++)
            {
                kos_sem_post(&g_probe_gate);
            }
            for (int i = 0; i < got; i++)
            {
                (void)probes[i].join();
            }
        }

        enum class ObjectKind
        {
            ENDPOINT,
            NOTIFY,
        };

        // Direct calls only: the trap red-zone gate refuses an indirect edge it cannot name.
        int make(ObjectKind kind, kos_cap_t* out)
        {
            switch (kind)
            {
            case ObjectKind::ENDPOINT:
            {
                return kos_endpoint_create(out);
            }
            case ObjectKind::NOTIFY:
            {
                return kos_notify_create(out);
            }
            }
            return -KOS_EINVAL;
        }

        void hold(int count, ObjectKind kind, char const* pool, kos_cap_t* held, int* n,
                  Refusal* r)
        {
            for (int i = 0; i < count and r->rc == 0; i++)
            {
                kos_cap_t h = KOS_CAP_NONE;
                note(r, make(kind, &h), pool, i, count);
                if (r->rc == 0)
                {
                    held[*n] = h;
                    *n = *n + 1;
                }
            }
        }
    }

    bool objects_refused(ObjectDemand want, kos_cap_t* line)
    {
        constexpr int MAX = 16;
        constexpr int TASKS_MAX = 8;
        constexpr int CAPS_MAX = 8;
        int total = want.endpoints + want.notifies;
        if (want.caps > 0 and want.notifies == 0)
        {
            total = total + 1;
        }
        if (total > MAX or want.tasks > TASKS_MAX or want.caps > CAPS_MAX
            or want.workers > POOL_PROBES_MAX)
        {
            tap::fail("an ask of %d workers, %d objects, %d tasks, %d caps is wider than its %d, "
                      "%d, %d and %d handles", want.workers, total, want.tasks, want.caps,
                      POOL_PROBES_MAX, MAX, TASKS_MAX, CAPS_MAX);
            return true;
        }
        int lines = want.irqs;
        if (lines == 0 and want.irq_line >= 0)
        {
            lines = 1;
        }
        Refusal r;
        if (lines > KICKOS_SELFTEST_IRQ_BINDINGS)
        {
            note(&r, -KOS_EAGAIN, "irqs", KICKOS_SELFTEST_IRQ_BINDINGS, lines);
        }
        if (r.rc == 0 and want.workers > 0)
        {
            workers_refusal(want.workers, &r);
        }
        kos_cap_t held[MAX];
        int n = 0;
        hold(want.endpoints, ObjectKind::ENDPOINT, "endpoints", held, &n, &r);
        hold(want.notifies, ObjectKind::NOTIFY, "notifies", held, &n, &r);
        kos_cap_t copies[CAPS_MAX];
        int c = 0;
        if (r.rc == 0 and want.caps > 0)
        {
            int source = want.endpoints;
            if (want.notifies == 0)
            {
                source = n;
                hold(1, ObjectKind::NOTIFY, "caps", held, &n, &r);
            }
            while (r.rc == 0 and c < want.caps)
            {
                note(&r, kos_notify_badge(held[source], static_cast<uint32_t>(c), &copies[c]),
                     "caps", c, want.caps);
                if (r.rc == 0)
                {
                    c = c + 1;
                }
            }
        }
        // The instances of stpeer are the tasks an arm creates, each holding its own record.
        if (r.rc == 0 and want.tasks > KICKOS_SELFTEST_PEERS + KICKOS_SELFTEST_SERVERS)
        {
            note(&r, -KOS_ENOMEM, "tasks", KICKOS_SELFTEST_PEERS + KICKOS_SELFTEST_SERVERS, want.tasks);
        }
        kos_cap_t claimed = KOS_CAP_NONE;
        if (r.rc == 0 and want.irq_line >= 0)
        {
            note(&r, irq_claim_await(want.irq_line, &claimed), "irq_line", 0, 1);
        }
        for (int i = 0; i < c; i++)
        {
            (void)kos_handle_close(copies[i]);
        }
        for (int i = 0; i < n; i++)
        {
            (void)kos_handle_close(held[i]);
        }
        if (r.rc == 0 and line != nullptr)
        {
            *line = claimed;
        }
        else if (claimed != KOS_CAP_NONE)
        {
            (void)kos_handle_close(claimed);
        }
        if (r.rc == 0)
        {
            return false;
        }
        if (r.rc != -KOS_EMFILE and r.rc != -KOS_EAGAIN and r.rc != -KOS_ENOMEM)
        {
            tap::fail("an ask's %s probe answered %d", r.pool, r.rc);
            return true;
        }
        if (KICKOS_SELFTEST_PEERS + KICKOS_SELFTEST_SERVERS == 0 and r.pool[0] == 't')
        {
            tap::skip("this image declares no peer");
            return true;
        }
        tap::skip("short of %s: %d of %d (%d)", r.pool, r.got, r.want, r.rc);
        return true;
    }

    int irq_claim_await(int line, kos_cap_t* out)
    {
        kos_monotonic_t const deadline = after_us(STALL_TOLERANT_US);
        int rc = kos_irq_claim(line, KOS_IRQ_EDGE, out);
        while (rc == -KOS_EAGAIN or rc == -KOS_ENOMEM)
        {
            if (kos_monotonic_now().ns > deadline.ns)
            {
                return rc;
            }
            kos_sleep_for(kos_ns(IRQ_EDGE_POLL_NS));
            rc = kos_irq_claim(line, KOS_IRQ_EDGE, out);
        }
        return rc;
    }

    ArmHold::Held* ArmHold::add(Kind kind, kos_cap_t* c)
    {
        if (n_ == MAX)
        {
            tap::fail("an ArmHold holds at most %d things", MAX);
            return nullptr;
        }
        Held* const h = &held_[n_];
        h->cap = c;
        h->space = nullptr;
        h->va = nullptr;
        h->kind = kind;
        n_ = n_ + 1;
        return h;
    }

    bool ArmHold::joined_until(kos_monotonic_t deadline)
    {
        bool all = true;
        for (int i = 0; i < n_; i++)
        {
            Held const& h = held_[i];
            if (h.kind != Kind::THREAD or not h.thread->valid())
            {
                continue;
            }
            int const rc = kos_thread_join(h.thread->id(), deadline);
            if (rc != 0 and rc != -KOS_EBADF)
            {
                all = false;
            }
        }
        return all;
    }

    bool ArmHold::joined()
    {
        return joined_until(after_us(bound_us_));
    }

    bool ArmHold::joined_rounds(Atomic<uint32_t, Order::RELAXED> const* rounds, int n,
                                Atomic<uint32_t, Order::RELAXED> const* stalls)
    {
        kos_monotonic_t const t0 = kos_monotonic_now();
        uint64_t last = 0;
        for (int p = 0; p < n; p++)
        {
            last += rounds[p].load();
        }
        while (true)
        {
            if (joined_until(after_us(STALL_TOLERANT_US)))
            {
                return true;
            }
            uint64_t sum = 0;
            for (int p = 0; p < n; p++)
            {
                sum += rounds[p].load();
            }
            if (sum != last)
            {
                last = sum;
                continue;
            }
            tap::diag("no round for %u ms, %u ms in",
                      static_cast<unsigned>(STALL_TOLERANT_US / 1000u),
                      static_cast<unsigned>(kos_between(t0, kos_monotonic_now()).ns / 1000000));
            for (int p = 0; p < n; p++)
            {
                if (stalls == nullptr)
                {
                    tap::diag("player %d: %u rounds", p, static_cast<unsigned>(rounds[p].load()));
                }
                else
                {
                    tap::diag("player %d: %u rounds, %u stalls", p,
                              static_cast<unsigned>(rounds[p].load()),
                              static_cast<unsigned>(stalls[p].load()));
                }
            }
            return false;
        }
    }

    void ArmHold::release()
    {
        // The death waits go through the arm's own bound notification where it holds one, so
        // they cost no object beyond the arm's ask.
        kos_cap_t bound = KOS_CAP_NONE;
        for (int i = 0; i < n_; i++)
        {
            Held const& h = held_[i];
            if (h.kind == Kind::OWNED)
            {
                (void)kos_mutex_unlock(h.mutex);
            }
            if (h.kind == Kind::BOUND and *h.cap != KOS_CAP_NONE and bound == KOS_CAP_NONE)
            {
                bound = *h.cap;
            }
        }
        for (int i = 0; i < n_; i++)
        {
            Held const& h = held_[i];
            if (h.kind != Kind::TASK or *h.task == KOS_TASK_NONE)
            {
                continue;
            }
            int const slain = slay_dead(*h.task, after_us(ARM_SLAY_US), bound);
            if (slain != 0)
            {
                tap::fail("ArmHold: slay of task %u answered %d", static_cast<unsigned>(*h.task),
                          slain);
            }
            *h.task = KOS_TASK_NONE;
        }
        for (int i = 0; i < n_; i++)
        {
            Held const& h = held_[i];
            if (h.kind == Kind::BOUND and *h.cap != KOS_CAP_NONE)
            {
                (void)kos_notify_unbind(*h.cap);
            }
        }
        kos_monotonic_t const deadline = after_us(bound_us_);
        for (int i = 0; i < n_; i++)
        {
            Held const& h = held_[i];
            if (h.kind != Kind::THREAD or not h.thread->valid())
            {
                continue;
            }
            int const joined = kos_thread_join(h.thread->id(), deadline);
            if (joined == 0 or joined == -KOS_EBADF)
            {
                continue;
            }
            int const slain = thread_slay_gone(h.thread);
            if (slain != 0 and slain != -KOS_EBADF)
            {
                tap::fail("ArmHold: slay of thread %u answered %d",
                          static_cast<unsigned>(h.thread->id()), slain);
            }
        }
#if KICKOS_HAVE_ASPACE
        for (int i = 0; i < n_; i++)
        {
            Held const& h = held_[i];
            if (h.kind == Kind::MAPPED and *h.va != 0 and *h.cap != KOS_CAP_NONE
                and *h.space != KOS_CAP_NONE)
            {
                (void)kos_frame_unmap(*h.cap, *h.space, *h.va);
            }
        }
#endif
        for (int i = 0; i < n_; i++)
        {
            Held const& h = held_[i];
            if (h.kind != Kind::THREAD and h.kind != Kind::TASK and h.kind != Kind::OWNED
                and *h.cap != KOS_CAP_NONE)
            {
                (void)close(h.cap);
            }
            if (h.kind == Kind::MAPPED and *h.space != KOS_CAP_NONE)
            {
                (void)close(h.space);
            }
        }
        n_ = 0;
    }

    namespace
    {
        RamNode* g_ram_first = nullptr;
        RamNode* g_ram_last = nullptr;

        size_t ram_align(size_t bytes)
        {
#if KICKOS_SELFTEST_FIT
            return st_region_align(bytes);
#else
            (void)bytes;
            return 1;
#endif
        }

#if KICKOS_SELFTEST_FIT
        // As many workers at once as the image's widest ask: on a bump arena their stacks come
        // before the first block, as arena_fit.py's ASSERT places them.
        bool seat_thread_stacks()
        {
            (void)kos_sem_init(&g_probe_gate, 0);
            kos::thread::Handle seated[KICKOS_SELFTEST_SEAT_WORKERS + 1];
            int n = 0;
            while (n < st_seat_workers and n < KICKOS_SELFTEST_SEAT_WORKERS)
            {
                seated[n] = kos::thread::create_caps(pool_gated_worker, nullptr, "seat", 10,
                                                     nullptr, 0);
                if (not seated[n].valid())
                {
                    break;
                }
                n++;
            }
            int const refused = seated[n].error();
            for (int i = 0; i < n; i++)
            {
                kos_sem_post(&g_probe_gate);
            }
            int unjoined = 0;
            for (int i = 0; i < n; i++)
            {
                if (seated[i].join_for(std::chrono::microseconds{STALL_TOLERANT_US}) != 0)
                {
                    unjoined++;
                }
            }
            if (n < st_seat_workers)
            {
                tap::bail_out("the seat spawned %d of %d workers, then %d", n, st_seat_workers,
                              refused);
                return false;
            }
            if (unjoined != 0)
            {
                tap::bail_out("%d seated worker(s) did not end", unjoined);
                return false;
            }
            return true;
        }
#endif
    }

    size_t arena_granule()
    {
#if KICKOS_HAVE_ASPACE
        return ASPACE_GRANULE;
#elif KICKOS_SELFTEST_FIT
        return ST_ARENA_GRANULE;
#else
        return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
    }

    namespace
    {
        size_t st_ram_bytes(uint32_t size)
        {
            if ((size & ST_GRANULE_UNIT) != 0)
            {
                return (size & ~ST_GRANULE_UNIT) * arena_granule();
            }
            return size;
        }
    }

    namespace
    {
        int g_registrations = 0;
    }

    bool st_next_left_out()
    {
        int const i = g_registrations;
        g_registrations = i + 1;
        return ((st_left_out[i / 8] >> (i % 8)) & 1u) != 0;
    }

    void st_ram_link(RamNode* node, char const* arm)
    {
        if (node->arm != nullptr)
        {
            return;
        }
        node->arm = arm;
        if (g_ram_last == nullptr)
        {
            g_ram_first = node;
        }
        else
        {
            g_ram_last->next = node;
        }
        g_ram_last = node;
    }

    namespace
    {
        // Largest alignment first, which the link ASSERT's sum assumes.
        bool reserve_asks()
        {
            size_t above = static_cast<size_t>(-1);
            while (true)
            {
                size_t align = 0;
                for (RamNode const* n = g_ram_first; n != nullptr; n = n->next)
                {
                    for (int i = 0; i < n->count; i++)
                    {
                        size_t const a = ram_align(st_ram_bytes(n->size[i]));
                        if (a < above and a > align)
                        {
                            align = a;
                        }
                    }
                }
                if (align == 0)
                {
                    return true;
                }
                for (RamNode* n = g_ram_first; n != nullptr; n = n->next)
                {
                    for (int i = 0; i < n->count; i++)
                    {
                        size_t const bytes = st_ram_bytes(n->size[i]);
                        if (ram_align(bytes) != align)
                        {
                            continue;
                        }
                        n->block[i] = kos_ram_alloc(bytes);
                        if (n->block[i] == nullptr)
                        {
                            tap::bail_out("the arena cannot back block %d of %s's ask (%lu bytes)",
                                          i, n->arm, static_cast<unsigned long>(bytes));
                            return false;
                        }
                        if ((reinterpret_cast<uintptr_t>(n->block[i]) & (align - 1u)) != 0)
                        {
                            tap::bail_out("block %d of %s's ask is not %lu-aligned", i, n->arm,
                                          static_cast<unsigned long>(align));
                            return false;
                        }
                    }
                }
                above = align;
            }
        }

#if KICKOS_HAVE_ASPACE
        uintptr_t xsem_granule()
        {
            void* const page = kos_ram_alloc(ASPACE_GRANULE);
            if (page == nullptr)
            {
                tap::bail_out("the semaphore page could not reserve its granule");
                return 0;
            }
            int const rc = st_self_grant(page, ASPACE_GRANULE, 0);
            if (rc != 0)
            {
                tap::bail_out("the semaphore page's grant answered %d", rc);
                return 0;
            }
            return reinterpret_cast<uintptr_t>(page);
        }
#endif
    }

    namespace
    {
#if KICKOS_HAVE_ASPACE
        uintptr_t g_xsem_page = 0;
#else
        kos_sem_t g_xsem[XSEM_SLOTS] = {};
#endif
    }

    kos_sem_t* xsem(int i)
    {
#if KICKOS_HAVE_ASPACE
        return reinterpret_cast<kos_sem_t*>(g_xsem_page) + i;
#else
        return &g_xsem[i];
#endif
    }

    kos_window xsem_window()
    {
        kos_window w = {};
#if KICKOS_HAVE_ASPACE
        w.base = g_xsem_page;
        w.size = static_cast<uint32_t>(ASPACE_GRANULE);
        w.kind = KOS_WINDOW_MEMORY;
#endif
        return w;
    }

    kos_sem_t* xsem_child(int i, uint16_t window)
    {
#if KICKOS_HAVE_ASPACE
        kos_window at = {};
        if (kos_window_get(window, &at) != 0)
        {
            return nullptr;
        }
        return reinterpret_cast<kos_sem_t*>(at.base) + i;
#else
        (void)window;
        return &g_xsem[i];
#endif
    }

    bool st_ram_seat()
    {
#if KICKOS_SELFTEST_FIT
        if (not seat_thread_stacks())
        {
            return false;
        }
#endif
#if KICKOS_HAVE_ASPACE
        g_xsem_page = xsem_granule();
        return g_xsem_page != 0 and reserve_asks();
#else
        return reserve_asks();
#endif
    }

    kos_cap_t g_pl_ep = KOS_CAP_NONE;

    kos_program_t g_peer_prog = KOS_CAP_NONE;

    namespace
    {
        int create_waiting(kos_program_t prog, kos_task_params const& p, kos_task_t* t,
                           kos_cap_t note)
        {
            kos_monotonic_t const until = after_us(STALL_TOLERANT_US);
            if (note == KOS_CAP_NONE)
            {
                return kickos::apps::create_when_startable(prog, p, until, t);
            }
            int const rc = kos_task_create(prog, &p, t);
            if (rc != -KOS_EAGAIN)
            {
                return rc;
            }
            kos_cap_t badged = KOS_CAP_NONE;
            if (kos_notify_badge(note, PEER_START_BIT, &badged) != 0)
            {
                return rc;
            }
            int const started
                = kickos::apps::create_when_startable(prog, p, until, badged, PEER_START_BIT, t);
            (void)kos_handle_close(badged);
            return started;
        }
    }

    int peer_start(kos_task_params const& p, kos_task_t* t, kos_program_t prog, kos_cap_t note)
    {
        if (p.instance != nullptr)
        {
            return create_waiting(prog, p, t, note);
        }
        // Named, so a peer whose last incarnation is still dying is waited out and never passed:
        // the arm's i-th task at once is on peer<i>, the budget its ask sized.
        // -KOS_EBUSY is also a busy block or window, refused on every peer alike.
        kos_task_params named = p;
        char name[] = "peer0";
        named.instance = name;
        for (int at = 0; at < SELFTEST_PEERS; at++)
        {
            name[4] = static_cast<char>('0' + at);
            int const rc = create_waiting(prog, named, t, note);
            if (rc == -KOS_ENOENT and at == 0)
            {
                return create_waiting(prog, p, t, note);
            }
            if (rc == -KOS_ENOENT)
            {
                break;
            }
            if (rc != -KOS_EBUSY)
            {
                return rc;
            }
        }
        return -KOS_EBUSY;
    }

    int peer_dead_status(kos_task_t t, uint32_t bound_us)
    {
        kos_monotonic_t const deadline = after_us(bound_us);
        while (true)
        {
            int status = 0;
            int const state = kos_task_state(t, &status);
            if (state >= 0 and (state & KOS_TASK_DEAD) != 0)
            {
                return status;
            }
            if (kos_monotonic_now().ns > deadline.ns)
            {
                return PEER_UNDEAD;
            }
            kos_sleep_for(kos_ms(1));
        }
    }

    PeerHex peer_hex(uintptr_t v)
    {
        static char const digits[] = "0123456789abcdef";
        PeerHex h = {};
        for (size_t i = 0; i < 2 * sizeof(uintptr_t); i++)
        {
            h.s[2 * sizeof(uintptr_t) - 1 - i] = digits[(v >> (4 * i)) & 0xFu];
        }
        return h;
    }

    uintptr_t peer_word(char const* s)
    {
        uintptr_t v = 0;
        for (; *s != '\0'; s++)
        {
            uintptr_t d = static_cast<uintptr_t>(*s - '0');
            if (*s >= 'a')
            {
                d = static_cast<uintptr_t>(*s - 'a' + 10);
            }
            v = v * 16u + d;
        }
        return v;
    }

#if KICKOS_SELFTEST_PEER
    void peer_seat(uint8_t prio, uint32_t core_mask)
    {
        if (core_mask != 0)
        {
            (void)kos_thread_affinity(kos_thread_self(), core_mask);
        }
        (void)kos_thread_set_priority(prio);
    }

    void peer_park(kos_cap_t n)
    {
        (void)kos_notify_bind(n);
        uint32_t bits = 0;
        (void)kos_notify_wait(0xFFFFFFFFu, KOS_NEVER, &bits);
    }
#endif

    bool peer_is(char const* got, char const* want)
    {
        if (got == nullptr)
        {
            return false;
        }
        size_t const n = strlen(want);
        return strlen(got) == n and memcmp(got, want, n) == 0;
    }
}
