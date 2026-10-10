// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The definitions every self-test translation unit reaches.

// The one translation unit that reserves: st_ram_seat.
#define KICKOS_SELFTEST_RAM_SEAT 1
#include "selftest.h"

#include <kickos/config/system.h> // KICKOS_TASK_IRQ_HANDLE_BUDGET
#include <kickos/config/priorities.h> // KICKOS_PRIO_MIN

namespace selftest
{
    kos_cap_t g_done = KOS_CAP_NONE; // shared completion counter (MAIN's cap; delegated to workers)
    kos_cap_t g_lock = KOS_CAP_NONE; // binary semaphore = mutex over the event log (MAIN's cap)
    kos_self_t const* g_self = nullptr;
    kos_thread_t g_main = KOS_THREAD_NONE;

    bool report_await(kos_cap_t ep, void* rep, size_t len)
    {
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        return kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, len), &opts)
               == static_cast<int>(len);
    }

    int thread_end(kos::thread::Handle* h, uint32_t bound_us)
    {
        int const rc = h->join(bound_us);
        if (rc != 0)
        {
            int const slain = h->slay(ARM_SLAY_US);
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

    int task_end(kos_task_t* t, uint32_t bound_us)
    {
        int const rc = kos_task_slay(*t, bound_us);
        if (rc != 0)
        {
            tap::fail("task %u: slay answered %d", static_cast<unsigned>(*t), rc);
            return rc;
        }
        *t = KOS_TASK_NONE;
        return rc;
    }

    namespace
    {
        // A sleep and not a yield: a poller above the thread it waits on would hold its core.
        constexpr uint64_t FLAG_STEP_NS = 500000ull;

        bool flag_reaches(Atomic<uint32_t, Order::RELAXED> const& cell, uint32_t value,
                          bool equal)
        {
            uint64_t const deadline = kos_clock_now() + uint64_t{STALL_TOLERANT_US} * 1000u;
            while ((cell.load() == value) != equal)
            {
                if (kos_clock_now() > deadline)
                {
                    return false;
                }
                kos_sleep_ns(FLAG_STEP_NS);
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

    // Above every batch an arm asks for: a crowd of one per kernel core plus one, or four.
    constexpr int POOL_PROBES_MAX = KICKOS_KERNEL_CORES + 4;

    void pool_gated_worker(void*) // caps: gate@1
    {
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
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
            kos_cap_t gate = KOS_CAP_NONE;
            int rc = kos_sem_create(0, &gate);
            if (rc != 0)
            {
                note(r, rc, "sems", 0, 1);
                return;
            }
            kos_cap_grant caps[] = {{gate, KOS_CAP_WAIT}};
            kos::thread::Handle probes[POOL_PROBES_MAX];
            int got = 0;
            for (int i = 0; i < n; i++)
            {
                probes[i] =
                    kos::thread::create_caps(pool_gated_worker, nullptr, "probe", 10, caps, 1);
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
                kos_sem_post(gate);
            }
            for (int i = 0; i < got; i++)
            {
                (void)probes[i].join();
            }
            (void)kos_handle_close(gate);
        }

        // Microseconds left to `deadline`, 0 once it has passed.
        uint32_t left_us(uint64_t deadline)
        {
            uint64_t const now = kos_clock_now();
            if (now >= deadline)
            {
                return 0;
            }
            return static_cast<uint32_t>((deadline - now) / 1000u);
        }

        enum class ObjectKind
        {
            SEM,
            MUTEX,
            ENDPOINT,
            NOTIFY,
        };

        // Direct calls only: the trap red-zone gate refuses an indirect edge it cannot name.
        int make(ObjectKind kind, kos_cap_t* out)
        {
            switch (kind)
            {
            case ObjectKind::SEM:
            {
                return kos_sem_create(0, out);
            }
            case ObjectKind::MUTEX:
            {
                return kos_mutex_create(out);
            }
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
        int total = want.sems + want.mutexes + want.endpoints + want.notifies;
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
        if (lines > KICKOS_TASK_IRQ_HANDLE_BUDGET)
        {
            note(&r, -KOS_EAGAIN, "irqs", KICKOS_TASK_IRQ_HANDLE_BUDGET, lines);
        }
        if (r.rc == 0 and want.workers > 0)
        {
            workers_refusal(want.workers, &r);
        }
        kos_cap_t held[MAX];
        int n = 0;
        hold(want.sems, ObjectKind::SEM, "sems", held, &n, &r);
        hold(want.mutexes, ObjectKind::MUTEX, "mutexes", held, &n, &r);
        hold(want.endpoints, ObjectKind::ENDPOINT, "endpoints", held, &n, &r);
        hold(want.notifies, ObjectKind::NOTIFY, "notifies", held, &n, &r);
        kos_cap_t copies[CAPS_MAX];
        int c = 0;
        if (r.rc == 0 and want.caps > 0)
        {
            int source = want.sems + want.mutexes + want.endpoints;
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
        kos_task_t tasks[TASKS_MAX];
        int t = 0;
        while (r.rc == 0 and t < want.tasks)
        {
            note(&r, kos_task_create(nullptr, 0, 0, &tasks[t]), "tasks", t, want.tasks);
            if (r.rc == 0)
            {
                t = t + 1;
            }
        }
        kos_cap_t claimed = KOS_CAP_NONE;
        if (r.rc == 0 and want.irq_line >= 0)
        {
            note(&r, irq_claim_await(want.irq_line, &claimed), "irq_line", 0, 1);
        }
        for (int i = 0; i < t; i++)
        {
            int const slain = kos_task_slay(tasks[i], ARM_SLAY_US);
            if (slain != 0)
            {
                tap::diag("an ask's probe task slay answered %d", slain);
            }
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
        tap::skip("short of %s: %d of %d (%d)", r.pool, r.got, r.want, r.rc);
        return true;
    }

    int irq_claim_await(int line, kos_cap_t* out)
    {
        uint64_t const deadline = kos_clock_now() + IRQ_EDGE_BUDGET_NS;
        int rc = kos_irq_claim(line, KOS_IRQ_EDGE, out);
        while (rc == -KOS_EAGAIN or rc == -KOS_ENOMEM)
        {
            if (kos_clock_now() > deadline)
            {
                return rc;
            }
            kos_sleep_ns(IRQ_EDGE_POLL_NS);
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

    bool ArmHold::joined()
    {
        uint64_t const deadline = kos_clock_now() + uint64_t{bound_us_} * 1000u;
        bool all = true;
        for (int i = 0; i < n_; i++)
        {
            Held const& h = held_[i];
            if (h.kind != Kind::THREAD or not h.thread->valid())
            {
                continue;
            }
            int const rc = h.thread->join(left_us(deadline));
            if (rc != 0 and rc != -KOS_EBADF)
            {
                all = false;
            }
        }
        return all;
    }

    void ArmHold::release()
    {
        for (int i = 0; i < n_; i++)
        {
            Held const& h = held_[i];
            if (h.kind == Kind::OWNED and *h.cap != KOS_CAP_NONE)
            {
                (void)kos_mutex_unlock(*h.cap);
            }
            if (h.kind == Kind::BOUND and *h.cap != KOS_CAP_NONE)
            {
                (void)kos_notify_unbind(*h.cap);
            }
        }
        for (int i = 0; i < n_; i++)
        {
            Held const& h = held_[i];
            if (h.kind != Kind::TASK or *h.task == KOS_TASK_NONE)
            {
                continue;
            }
            int const slain = kos_task_slay(*h.task, ARM_SLAY_US);
            if (slain == 0)
            {
                *h.task = KOS_TASK_NONE;
            }
            else
            {
                tap::fail("ArmHold: slay of task %u answered %d", static_cast<unsigned>(*h.task),
                          slain);
            }
        }
        uint64_t const deadline = kos_clock_now() + uint64_t{bound_us_} * 1000u;
        for (int i = 0; i < n_; i++)
        {
            Held const& h = held_[i];
            if (h.kind != Kind::THREAD or not h.thread->valid())
            {
                continue;
            }
            int const joined = h.thread->join(left_us(deadline));
            if (joined == 0 or joined == -KOS_EBADF)
            {
                continue;
            }
            int const slain = h.thread->slay(ARM_SLAY_US);
            if (slain != 0)
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
            if (h.kind != Kind::THREAD and h.kind != Kind::TASK and *h.cap != KOS_CAP_NONE)
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
#if not KICKOS_HAVE_ASPACE and not KICKOS_SELFTEST_FIT
        size_t g_granule = 0;
#endif

        size_t ram_align(size_t bytes)
        {
#if KICKOS_SELFTEST_FIT
            return st_region_align(bytes);
#else
            (void)bytes;
            return 1;
#endif
        }

#if KICKOS_HAVE_ASPACE
        void seat_noop(void*) {}

        // The page tables main's first memory window needs stay in its space after the holder
        // ends: seated here, before any arm's census.
        bool seat_window_tables()
        {
            void* const blk = kos_ram_alloc(ASPACE_GRANULE);
            if (blk == nullptr)
            {
                tap::bail_out("the seat could not reserve the window's block");
                return false;
            }
            kos_window const w = {reinterpret_cast<uintptr_t>(blk), ASPACE_GRANULE,
                                  KOS_WINDOW_MEMORY, 0};
            kos::thread::Handle t = kos::thread::create(seat_noop, nullptr, "seatw", 10,
                                                        KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                        nullptr, 0, &w, 1);
            if (not t.valid())
            {
                tap::bail_out("the seat's window spawn answered %d", t.error());
                return false;
            }
            int const joined = t.join(STALL_TOLERANT_US);
            if (joined != 0)
            {
                tap::bail_out("the seat's window holder joined %d", joined);
                return false;
            }
            return true;
        }
#endif

#if KICKOS_SELFTEST_FIT or KICKOS_HAVE_ASPACE
        // As many workers at once as the image's widest ask: on a bump arena their stacks come
        // before the first block, as arena_fit.py's ASSERT places them; under translation the
        // page tables their stacks need stay in main's space.
        bool seat_thread_stacks()
        {
            kos_cap_t gate = KOS_CAP_NONE;
            int const made = kos_sem_create(0, &gate);
            if (made != 0)
            {
                tap::bail_out("the seat's gate answered %d", made);
                return false;
            }
            kos_cap_grant caps[] = {{gate, KOS_CAP_WAIT}};
            kos::thread::Handle seated[KICKOS_SELFTEST_SEAT_WORKERS + 1];
            int n = 0;
            while (n < st_seat_workers and n < KICKOS_SELFTEST_SEAT_WORKERS)
            {
                seated[n] = kos::thread::create_caps(pool_gated_worker, nullptr, "seat", 10, caps,
                                                     1);
                if (not seated[n].valid())
                {
                    break;
                }
                n++;
            }
            int const refused = seated[n].error();
            for (int i = 0; i < n; i++)
            {
                kos_sem_post(gate);
            }
            int unjoined = 0;
            for (int i = 0; i < n; i++)
            {
                if (seated[i].join(STALL_TOLERANT_US) != 0)
                {
                    unjoined++;
                }
            }
            (void)kos_handle_close(gate);
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
        return g_granule;
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
    }

    bool st_ram_seat()
    {
#if KICKOS_SELFTEST_FIT
        if (not seat_thread_stacks())
        {
            return false;
        }
#endif
#if not KICKOS_HAVE_ASPACE and not KICKOS_SELFTEST_FIT
        void* const p = kos_ram_alloc(1);
        void* const q = kos_ram_alloc(1);
        if (p == nullptr or q == nullptr or q <= p)
        {
            tap::bail_out("the arena spares no two blocks to measure its granule");
            return false;
        }
        g_granule = static_cast<size_t>(static_cast<char*>(q) - static_cast<char*>(p));
#endif
        if (not reserve_asks())
        {
            return false;
        }
#if KICKOS_HAVE_ASPACE
        // After the blocks, on the frames later stacks are taken from.
        if (not seat_thread_stacks() or not seat_window_tables())
        {
            return false;
        }
#endif
        return true;
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    kos_cap_t g_pl_ep = KOS_CAP_NONE;
#endif
}
