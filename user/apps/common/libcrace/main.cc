// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Four threads, two in main's task and two in a task of their own, print through stdio and
// allocate from the heap at once, each preempted every few tens of microseconds. Every line a
// worker prints is one known text, and every block it allocates holds its pattern until freed.
// On a region board the two tasks share one heap and one stdout; on a translating one each task
// has its own, shared by its two threads. Main's task's workers start on a heap main has
// exhausted, and a round the heap cannot serve waits for it. Before they start: a thread killed
// while it waits for the malloc lock ends alone, and so does one that holds the libc lock and
// hands it over; a killed waiter asks again at the kernel and ends there; a holder killed while
// it runs releases the malloc lock; getenv with a kill pending takes the free libc lock with no
// kernel entry; a malloc lock taken from a holder that died holding it ends the taker's task;
// and a printf completes while another thread of main's task holds the malloc lock.

#include <kickos/config/priorities.h>
#include <kickos/kos.h>
#include <kickos/sys.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_NEWLIB_VERSION)
// newlib ships this header with no C++ linkage of its own.
extern "C"
{
#include <envlock.h>
}
#endif

namespace
{
    constexpr int WORKERS = 4;
    constexpr int ROUNDS = 300;
    constexpr int PAYLOAD = 40;
    constexpr int BLOCKS = 3;
    constexpr uint32_t QUANTUM_NS = 20000;
    constexpr uint8_t WORKER_PRIO = 10;
    constexpr uint32_t PRINT_WAIT_US = 3000000;
    constexpr uint8_t VICTIM_PRIO = KICKOS_PRIO_MAX;

    // The semaphores between main and the threads it spawns. A task of a translating board holds
    // its own copy of every static, so there they lie in a granule main reserves, which a thread
    // of another task reaches through the first window of its spawn.
    struct Gates
    {
        kos_sem_t done;
        kos_sem_t starved;
        kos_sem_t held;
        kos_sem_t release;
        kos_sem_t entering;
        kos_sem_t printed;
        kos_sem_t go;
    };
    Gates g_static_gates = {};
    Gates* g_gates = &g_static_gates;
#if KICKOS_HAVE_ASPACE
    constexpr uint32_t GATES_BYTES = 4096;
#endif

    Gates* gates()
    {
#if KICKOS_HAVE_ASPACE
        kos_window w = {};
        if (kos_window_get(0, &w) == 0)
        {
            return reinterpret_cast<Gates*>(w.base);
        }
#endif
        return g_gates;
    }

    // Every gate at 0, in main before any thread of the run exists.
    bool gates_seat()
    {
#if KICKOS_HAVE_ASPACE
        void* const page = kos_ram_alloc(GATES_BYTES);
        if (page == nullptr or kos_mem_self_grant(page, GATES_BYTES, 0) != 0)
        {
            return false;
        }
        g_gates = static_cast<Gates*>(page);
#endif
        kos_sem_t* const all[] = {&g_gates->done, &g_gates->starved, &g_gates->held,
                                  &g_gates->release, &g_gates->entering, &g_gates->printed,
                                  &g_gates->go};
        for (kos_sem_t* const g : all)
        {
            (void)kos_sem_init(g, 0);
        }
        return true;
    }

    // What a spawn into `task` passes for the gates: nothing for main's own task.
    uint16_t gates_window(kos_task_t task, kos_window* w)
    {
        *w = {};
#if KICKOS_HAVE_ASPACE
        if (task != KOS_TASK_NONE)
        {
            w->base = reinterpret_cast<uintptr_t>(g_gates);
            w->size = GATES_BYTES;
            w->kind = KOS_WINDOW_MEMORY;
            return 1;
        }
#else
        (void)task;
#endif
        return 0;
    }

    // `entry` in `task`, reaching the gates.
    kos::thread::Handle spawn_in(void (*entry)(void*), void* arg, char const* name, uint8_t prio,
                                 kos_task_t task, uint32_t core_mask,
                                 uint8_t policy = KOS_POLICY_FIFO, uint32_t quantum_ns = 0)
    {
        kos_window w;
        uint16_t const windows = gates_window(task, &w);
        return kos::thread::create(entry, arg, name, prio, policy, quantum_ns, nullptr, 0,
                                   nullptr, 0, &w, windows, nullptr, 0, 0, nullptr, task,
                                   core_mask);
    }

    char letter(int w, int i)
    {
        return static_cast<char>('a' + (w * 7 + i) % 26);
    }

    bool whole(char const* block, size_t len, char c)
    {
        for (size_t k = 0; k < len; k++)
        {
            if (block[k] != c)
            {
                return false;
            }
        }
        return true;
    }

    // The sim's host libc has no lock to hold.
    void malloc_holder(void*)
    {
#if defined(_NEWLIB_VERSION)
        __malloc_lock(_REENT);
#endif
        kos_sem_post(&gates()->held);
        kos_sem_wait(&gates()->release, KOS_TIMEOUT_NONE);
#if defined(_NEWLIB_VERSION)
        __malloc_unlock(_REENT);
#endif
    }

    void malloc_victim(void*)
    {
        kos_sem_post(&gates()->entering);
        void* volatile block = malloc(32);
        free(block);
    }

    // The env lock is the libc lock.
    void libc_holder_victim(void*)
    {
#if defined(_NEWLIB_VERSION)
        __env_lock(_REENT);
#endif
        kos_sem_post(&gates()->entering);
        void* volatile block = malloc(32);
        free(block);
#if defined(_NEWLIB_VERSION)
        __env_unlock(_REENT);
#endif
    }

#if defined(_NEWLIB_VERSION) or (KICKOS_FAULT_ISOLATION and KICKOS_HAVE_ASPACE)
    void anchor(void*)
    {
        while (true)
        {
            kos::sleep_ns(1000000000ull);
        }
    }
#endif

#if KICKOS_FAULT_ISOLATION and KICKOS_HAVE_ASPACE
    // Never unlocks: a kill must find it at the re-wait, holding the malloc lock.
    void dead_malloc_holder(void*)
    {
#if defined(_NEWLIB_VERSION)
        __malloc_lock(_REENT);
#endif
        Gates* const g = gates();
        kos_sem_post(&g->held);
        while (true)
        {
            (void)kos_sem_wait(&g->release, KOS_TIMEOUT_NONE);
        }
    }
#endif

    void printer(void*)
    {
        printf("libcrace: printf beside the malloc lock %d\n", 1);
        kos_sem_post(&gates()->printed);
    }

#if defined(_NEWLIB_VERSION)
    uint32_t g_core_mask = 0;
    uint32_t g_reask_took = 0;
    bool g_reask_ran = false;
    uint32_t g_release_go = 0;
    uint32_t g_env_after_kill = 0;
    char const* volatile g_env = nullptr;

    void reask_victim(void*)
    {
        kos_sem_post(&gates()->entering);
        void* volatile block = malloc(32);
        __atomic_store_n(&g_reask_took, 1u, __ATOMIC_RELAXED);
        free(block);
    }

    // At the victim's priority, so neither the victim's post nor its kill hands it the core: the
    // victim, killed while it waits, runs again only once this holder has freed the lock.
    void reask_holder(void*)
    {
        __malloc_lock(_REENT);
        auto victim = spawn_in(reask_victim, nullptr, "mreask", VICTIM_PRIO, KOS_TASK_NONE,
                               g_core_mask);
        bool parked = victim.valid() and kos_sem_wait(&g_gates->entering, KOS_TIMEOUT_NONE) == 0;
        bool const killed = victim.valid() and victim.cancel(KOS_CANCEL_KILL, 0) == 0;
        __malloc_unlock(_REENT);
        g_reask_ran = parked and killed and victim.join() == 0;
    }

    // Takes the malloc lock with no entry, then spins with none, so a kill finds it running.
    void running_holder(void*)
    {
        __malloc_lock(_REENT);
        kos_sem_post(&gates()->held);
        while (__atomic_load_n(&g_release_go, __ATOMIC_ACQUIRE) == 0)
        {
        }
        __malloc_unlock(_REENT);
        while (true)
        {
            kos_yield();
        }
    }

    // In the holder's task, whose copy of the flag the holder reads.
    void release_setter(void*)
    {
        (void)kos_sem_wait(&gates()->go, KOS_TIMEOUT_NONE);
        __atomic_store_n(&g_release_go, 1u, __ATOMIC_RELEASE);
    }

    void getenv_after_kill(void*)
    {
        kos_sem_post(&gates()->entering);
        (void)kos_sem_wait(&gates()->release, KOS_TIMEOUT_NONE);
        g_env = getenv("KICKOS_LIBCRACE_UNSET");
        __atomic_store_n(&g_env_after_kill, 1u, __ATOMIC_RELAXED);
        kos_yield();
    }
#endif

    // Holds the malloc lock in a thread of main's task, ended by end_hold.
    bool hold_malloc(kos::thread::Handle* holder)
    {
        (void)kos_sem_init(&g_gates->held, 0);
        (void)kos_sem_init(&g_gates->release, 0);
        *holder = kos::thread::create(malloc_holder, nullptr, "mholder", WORKER_PRIO);
        bool holding = holder->valid();
        if (holding and kos_sem_wait(&g_gates->held, KOS_TIMEOUT_NONE) != 0)
        {
            holding = false;
            kos_sem_post(&g_gates->release);
            holder->join();
        }
        return holding;
    }

    void end_hold(kos::thread::Handle const& holder)
    {
        kos_sem_post(&g_gates->release);
        holder.join();
    }

    // Pins main to the core it runs on. A one-core kernel has nothing to pin; any other refusal
    // refuses the arm, which would otherwise kill a victim that had not parked yet.
    bool pin_main(uint32_t* core_mask)
    {
        int const core = kos_core_current();
        int const rc = kos_thread_affinity(kos_thread_self(), 1u << core);
        *core_mask = 0;
        if (rc == -KOS_ENOSYS)
        {
            return true;
        }
        if (rc < 0)
        {
            return false;
        }
        *core_mask = 1u << core;
        return true;
    }

    void unpin_main(uint32_t core_mask)
    {
        if (core_mask != 0)
        {
            kos_thread_affinity(kos_thread_self(), 0);
        }
    }

    // The victim outranks main on main's core, so main, woken by its post, runs again only once
    // it has parked. Invalid when the spawn or the post fails.
    kos::thread::Handle spawn_parked(void (*entry)(void*), char const* name, kos_task_t task,
                                     uint32_t core_mask)
    {
        (void)kos_sem_init(&g_gates->entering, 0);
        auto victim = spawn_in(entry, nullptr, name, VICTIM_PRIO, task, core_mask);
        if (victim.valid() and kos_sem_wait(&g_gates->entering, KOS_TIMEOUT_NONE) != 0)
        {
            victim.cancel(KOS_CANCEL_KILL, 0);
            victim.join();
            return kos::thread::Handle();
        }
        return victim;
    }

    // `entry` parks inside malloc, behind a holder of the malloc lock, and is killed there.
    bool kill_inside_malloc(void (*entry)(void*), char const* name)
    {
        kos::thread::Handle holder;
        uint32_t core_mask = 0;
        if (not pin_main(&core_mask))
        {
            return false;
        }
        bool ended = false;
        if (hold_malloc(&holder))
        {
            auto victim = spawn_parked(entry, name, KOS_TASK_NONE, core_mask);
            ended = victim.valid() and victim.cancel(KOS_CANCEL_KILL, 0) == 0
                    and victim.join() == 0;
            end_hold(holder);
        }
        unpin_main(core_mask);
        return ended;
    }

    // The killed holder's death reaches the next take of the libc lock, which holds it; the host
    // libc of the sim takes no lock of ours.
    bool libc_lock_handed_over()
    {
#if defined(_NEWLIB_VERSION)
        kos_mutex_t* const lock = kos_libc_lock(KOS_LIBC_LOCK_STDIO);
        if (kos_mutex_lock(lock, 0) != -KOS_EOWNERDEAD)
        {
            return false;
        }
        return kos_mutex_unlock(lock) == 0;
#else
        return true;
#endif
    }

#if defined(_NEWLIB_VERSION)
    // A waiter of the malloc lock is killed and its holder frees the lock before the waiter runs
    // again: *asked when the waiter ended at its next entry without taking it.
    bool killed_waiter_asks_the_kernel(bool* asked)
    {
        *asked = false;
        uint32_t core_mask = 0;
        if (not pin_main(&core_mask))
        {
            return false;
        }
        (void)kos_sem_init(&g_gates->entering, 0);
        g_core_mask = core_mask;
        g_reask_took = 0;
        g_reask_ran = false;
        auto holder = spawn_in(reask_holder, nullptr, "mholder", VICTIM_PRIO, KOS_TASK_NONE,
                               core_mask);
        bool const ran = holder.valid() and holder.join() == 0 and g_reask_ran;
        *asked = __atomic_load_n(&g_reask_took, __ATOMIC_RELAXED) == 0;
        unpin_main(core_mask);
        return ran;
    }

    // In a task of its own, whose entry only anchors it: a holder of the malloc lock is killed
    // while it runs, a taker parks behind it, and the holder releases. *lived when the taker took
    // the lock and its task did not end.
    bool running_killed_holder_releases(bool* lived)
    {
        *lived = false;
        uint32_t core_mask = 0;
        if (not pin_main(&core_mask))
        {
            return false;
        }
        kos_task_t task = KOS_TASK_NONE;
        (void)kos_sem_init(&g_gates->held, 0);
        (void)kos_sem_init(&g_gates->go, 0);
        bool ran = kos_task_create(nullptr, 0, 0, &task) == 0
                   and kos_task_sched_grant(task, VICTIM_PRIO, 0) == 0;
        kos::thread::Handle base;
        kos::thread::Handle holder;
        kos::thread::Handle setter;
        if (ran)
        {
            base = kos::thread::create_caps(anchor, nullptr, "manchor", WORKER_PRIO, nullptr, 0,
                                            KOS_POLICY_FIFO, 0, nullptr, 0, 0, nullptr, task);
            setter = spawn_in(release_setter, nullptr, "mgo", WORKER_PRIO, task, core_mask);
            // Below main, so it holds the lock and spins only while main waits.
            holder = spawn_in(running_holder, nullptr, "mrunner", KICKOS_PRIO_MIN, task,
                              core_mask);
            ran = base.valid() and setter.valid() and holder.valid()
                  and kos_sem_wait(&g_gates->held, KOS_TIMEOUT_NONE) == 0
                  and holder.cancel(KOS_CANCEL_KILL, 0) == 0
                  and kos_sem_post(&g_gates->go) == 0 and setter.join() == 0;
        }
        if (ran)
        {
            auto taker = spawn_parked(malloc_victim, "mtaker", task, core_mask);
            ran = taker.valid() and taker.join() == 0 and holder.join() == 0;
        }
        if (ran)
        {
            // A live task leaves the status untouched.
            int status = -99;
            int const state = kos_task_state(task, &status);
            *lived = state >= 0 and (state & KOS_TASK_ENDED) == 0 and status == -99;
        }
        if (task != KOS_TASK_NONE)
        {
            kos_task_slay(task, 0);
        }
        if (base.valid())
        {
            base.join();
        }
        if (holder.valid())
        {
            holder.join();
        }
        if (setter.valid())
        {
            setter.join();
        }
        unpin_main(core_mask);
        return ran;
    }

    // A thread killed out of its park reads the environment, whose lock is free: *read when it
    // came back from getenv before it died.
    bool getenv_with_a_kill_pending(bool* read)
    {
        *read = false;
        uint32_t core_mask = 0;
        if (not pin_main(&core_mask))
        {
            return false;
        }
        (void)kos_sem_init(&g_gates->entering, 0);
        (void)kos_sem_init(&g_gates->release, 0);
        g_env_after_kill = 0;
        // The child outranks main on main's core, so main, woken by its post, runs again only
        // once it has parked on the release gate.
        auto child = spawn_in(getenv_after_kill, nullptr, "menv", VICTIM_PRIO, KOS_TASK_NONE,
                              core_mask);
        bool const ran = child.valid() and kos_sem_wait(&g_gates->entering, KOS_TIMEOUT_NONE) == 0
                         and child.cancel(KOS_CANCEL_KILL, 0) == 0 and child.join() == 0;
        *read = __atomic_load_n(&g_env_after_kill, __ATOMIC_RELAXED) != 0;
        unpin_main(core_mask);
        return ran;
    }
#endif

#if KICKOS_FAULT_ISOLATION and KICKOS_HAVE_ASPACE
    // In a task of its own, whose entry only anchors it: a holder of the malloc lock is killed,
    // with a taker parked behind it or, `waiting` false, one that comes only after the death, so
    // the taker takes the lock from a dead owner.
    bool dead_malloc_holder_ends_the_takers_task(bool waiting, bool* ended)
    {
        *ended = false;
        uint32_t core_mask = 0;
        if (not pin_main(&core_mask))
        {
            return false;
        }
        kos_task_t task = KOS_TASK_NONE;
        (void)kos_sem_init(&g_gates->held, 0);
        (void)kos_sem_init(&g_gates->release, 0);
        bool ran = kos_task_create(nullptr, 0, 0, &task) == 0
                   and kos_task_sched_grant(task, VICTIM_PRIO, 0) == 0;
        kos::thread::Handle base;
        kos::thread::Handle holder;
        if (ran)
        {
            base = kos::thread::create_caps(anchor, nullptr, "manchor", WORKER_PRIO, nullptr, 0,
                                            KOS_POLICY_FIFO, 0, nullptr, 0, 0, nullptr, task);
            holder = spawn_in(dead_malloc_holder, nullptr, "mholder", WORKER_PRIO, task, 0);
            ran = base.valid() and holder.valid()
                  and kos_sem_wait(&g_gates->held, KOS_TIMEOUT_NONE) == 0;
        }
        if (ran and waiting)
        {
            auto taker = spawn_parked(malloc_victim, "mwaiter", task, core_mask);
            ran = taker.valid() and holder.cancel(KOS_CANCEL_KILL, 0) == 0 and holder.join() == 0
                  and taker.join() == 0;
        }
        else if (ran)
        {
            ran = holder.cancel(KOS_CANCEL_KILL, 0) == 0 and holder.join() == 0;
            auto taker = spawn_parked(malloc_victim, "mlater", task, core_mask);
            ran = ran and taker.valid() and taker.join() == 0;
        }
        if (ran)
        {
            int status = 0;
            int const state = kos_task_state(task, &status);
            *ended = state >= 0 and (state & KOS_TASK_ENDED) != 0 and status == KOS_EXIT_FAULT;
        }
        if (task != KOS_TASK_NONE)
        {
            kos_task_slay(task, 0);
        }
        if (base.valid())
        {
            base.join();
        }
        if (holder.valid())
        {
            holder.join();
        }
        unpin_main(core_mask);
        return ran;
    }
#endif

    // The printf allocates nothing: main's first printf gave stdout its buffer.
    bool print_beside_malloc()
    {
        kos::thread::Handle holder;
        (void)kos_sem_init(&g_gates->printed, 0);
        if (not hold_malloc(&holder))
        {
            return false;
        }
        auto print = kos::thread::create(printer, nullptr, "mprint", WORKER_PRIO);
        bool const started = print.valid();
        if (started and kos_sem_wait(&g_gates->printed, PRINT_WAIT_US) == 0)
        {
            kos::print("libcrace: a printf completed while malloc was held\n");
        }
        else
        {
            kos::print("libcrace: a printf waited behind the malloc lock\n");
        }
        end_hold(holder);
        if (started)
        {
            print.join();
        }
        return started;
    }

    struct Ballast
    {
        Ballast* next;
    };

    // The sim's host heap has no end to reach.
    Ballast* exhaust_heap()
    {
        Ballast* head = nullptr;
#if defined(_NEWLIB_VERSION)
        size_t size = 4096;
        while (size >= sizeof(Ballast))
        {
            auto* const b = static_cast<Ballast*>(malloc(size));
            if (b == nullptr)
            {
                size /= 2;
                continue;
            }
            b->next = head;
            head = b;
        }
#endif
        return head;
    }

    void release_heap(Ballast* head)
    {
        while (head != nullptr)
        {
            Ballast* const next = head->next;
            free(head);
            head = next;
        }
    }

    // A heap the workers' blocks exhaust answers NULL, which is not a torn block: the round
    // gives back what it took and asks again.
    bool take_blocks(char* (&blocks)[BLOCKS], size_t const (&sizes)[BLOCKS], char c)
    {
        for (int b = 0; b < BLOCKS; b++)
        {
            blocks[b] = static_cast<char*>(malloc(sizes[b]));
            if (blocks[b] == nullptr)
            {
                for (int k = 0; k < b; k++)
                {
                    free(blocks[k]);
                    blocks[k] = nullptr;
                }
                return false;
            }
            memset(blocks[b], c, sizes[b]);
        }
        return true;
    }

    void worker(void* arg)
    {
        int const w = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        Gates* const g = gates();
        int torn = 0;
        bool starved = false;
        for (int i = 0; i < ROUNDS; i++)
        {
            char const c = letter(w, i);
            size_t const sizes[BLOCKS] = {16u + static_cast<size_t>((i * 13) % 96), 64u, 24u};
            char* blocks[BLOCKS] = {};
            while (not take_blocks(blocks, sizes, c))
            {
                if (not starved)
                {
                    starved = true;
                    kos_sem_post(&g->starved);
                }
                kos::sleep_ns(QUANTUM_NS);
            }
            char payload[PAYLOAD + 1];
            memset(payload, c, PAYLOAD);
            payload[PAYLOAD] = '\0';
            printf("libcrace w%d i%d %s\n", w, i, payload);
            for (int b = 0; b < BLOCKS; b++)
            {
                if (not whole(blocks[b], sizes[b], c))
                {
                    torn++;
                }
                free(blocks[b]);
            }
        }
        printf("libcrace w%d done torn=%d\n", w, torn);
        kos_sem_post(&g->done);
        // The other task's entry returning would end its sibling with it.
        while (true)
        {
            kos::sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
    printf("libcrace: %d workers x %d rounds\n", WORKERS, ROUNDS);
    if (not gates_seat())
    {
        kos::print("libcrace: setup refused\n");
        return 1;
    }
    if (not kill_inside_malloc(malloc_victim, "mvictim"))
    {
        kos::print("libcrace: the malloc kill arm was refused\n");
        return 1;
    }
    kos::print("libcrace: a thread killed waiting for malloc ended alone\n");
    if (not kill_inside_malloc(libc_holder_victim, "mlibc") or not libc_lock_handed_over())
    {
        kos::print("libcrace: a thread killed holding the libc lock kept it\n");
        return 1;
    }
    kos::print("libcrace: a thread killed holding the libc lock handed it over\n");
#if defined(_NEWLIB_VERSION)
    bool asked = false;
    if (not killed_waiter_asks_the_kernel(&asked))
    {
        kos::print("libcrace: the killed waiter's re-ask arm was refused\n");
        return 1;
    }
    if (asked)
    {
        kos::print("libcrace: a killed malloc waiter asked again at the kernel and ended there\n");
    }
    else
    {
        kos::print("libcrace: a killed malloc waiter took the lock freed under it\n");
    }
    bool lived = false;
    if (not running_killed_holder_releases(&lived))
    {
        kos::print("libcrace: the running killed holder arm was refused\n");
        return 1;
    }
    if (lived)
    {
        kos::print("libcrace: a holder killed while running released malloc, and its taker's "
                   "task lived\n");
    }
    else
    {
        kos::print("libcrace: a holder killed while running died holding malloc, and its taker's "
                   "task ended\n");
    }
    bool read = false;
    if (not getenv_with_a_kill_pending(&read))
    {
        kos::print("libcrace: the getenv kill arm was refused\n");
        return 1;
    }
    if (read)
    {
        kos::print("libcrace: getenv with a kill pending took the free libc lock with no entry\n");
    }
    else
    {
        kos::print("libcrace: getenv with a kill pending died at the libc lock\n");
    }
#endif
#if KICKOS_FAULT_ISOLATION and KICKOS_HAVE_ASPACE
    bool dead_ended = false;
    if (not dead_malloc_holder_ends_the_takers_task(true, &dead_ended))
    {
        kos::print("libcrace: the dead malloc holder arm was refused\n");
        return 1;
    }
    if (dead_ended)
    {
        kos::print("libcrace: a malloc lock taken from a dead holder ended its task\n");
    }
    else
    {
        kos::print("libcrace: a malloc lock taken from a dead holder did not end its task\n");
    }
    if (not dead_malloc_holder_ends_the_takers_task(false, &dead_ended))
    {
        kos::print("libcrace: the unwatched dead malloc holder arm was refused\n");
        return 1;
    }
    if (dead_ended)
    {
        kos::print("libcrace: a malloc lock its holder died in unwatched ended the next taker's "
                   "task\n");
    }
    else
    {
        kos::print("libcrace: a malloc lock its holder died in unwatched let the next taker "
                   "run\n");
    }
#else
    kos::print("libcrace: no task-private heap with a contained fault, so the dead malloc holder "
               "arms did not run\n");
#endif
    if (not print_beside_malloc())
    {
        kos::print("libcrace: the printf beside malloc arm was refused\n");
        return 1;
    }
    kos_task_t other = KOS_TASK_NONE;
    if (kos_task_create(nullptr, 0, 0, &other) != 0)
    {
        kos::print("libcrace: setup refused\n");
        return 1;
    }
    Ballast* const ballast = exhaust_heap();
    int started = 0;
    for (int w = 0; w < WORKERS; w++)
    {
        kos_task_t task = KOS_TASK_NONE;
        if (w >= WORKERS / 2)
        {
            task = other;
        }
        auto h = spawn_in(worker, reinterpret_cast<void*>(static_cast<uintptr_t>(w)), "racer",
                          WORKER_PRIO, task, 0, KOS_POLICY_RR, QUANTUM_NS);
        if (h.valid())
        {
            started++;
        }
    }
    if (ballast != nullptr and started == WORKERS)
    {
        kos_sem_wait(&g_gates->starved, KOS_TIMEOUT_NONE);
    }
    release_heap(ballast);
    for (int w = 0; w < started; w++)
    {
        kos_sem_wait(&g_gates->done, KOS_TIMEOUT_NONE);
    }
    if (started != WORKERS)
    {
        kos::print("libcrace: spawn refused\n");
        return 1;
    }
    kos::print("libcrace: all workers done\n");
    return 0;
}
