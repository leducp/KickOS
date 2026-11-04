// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Four threads, two in main's task and two in the task of a peer program (peer.cc), print through
// stdio and allocate from the heap at once, each preempted every few tens of microseconds. Every
// line a worker prints is one known text, and every block it allocates holds its pattern until
// freed. Each task has its own heap and its own stdout, shared by its two threads, and the two
// tasks' lines meet only on the console. A line leaves in one write where stdout is line-buffered;
// where it is unbuffered (microbit) the two tasks' lines mix there. Main's task's workers start on
// a heap main has exhausted to the end of its window, and a round the heap cannot serve waits for
// it. Before they start: a thread killed while it waits for the malloc lock ends alone, and so
// does one that holds the libc lock and hands it over; a killed waiter asks again at the kernel
// and ends there; a holder killed while it runs releases the malloc lock; getenv with a kill
// pending takes the free libc lock with no kernel entry; a malloc lock taken from a holder that
// died holding it ends the taker's task; and a printf completes while another thread of main's
// task holds the malloc lock.

#include <kickos/apps/peer_run.h>
#include <kickos/chip_limits.h> // KICKOS_CHIP_ONE_TASK
#include <kickos/kos.h>
#include <kickos/sys.h>

#include "libcrace.h"

#if defined(_NEWLIB_VERSION)
// The image's heap window and its break (user/src/newlib_sbrk.cc).
extern "C" char _kickos_heap_start[];
extern "C" char _kickos_heap_limit[];
extern "C" void* _sbrk(intptr_t incr);
#endif

namespace
{
    using namespace libcrace;

    // The racing workers of main's task: the others run in the peer's, where there is one.
    constexpr int MAIN_WORKERS = static_cast<int>(TASK_RACERS);

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

    void printer(void*)
    {
        printf("libcrace: printf beside the malloc lock %d\n", 1);
        kos_sem_post(&gates()->printed);
    }

#if defined(_NEWLIB_VERSION)
    uint32_t g_core_mask = 0;
    uint32_t g_reask_took = 0;
    bool g_reask_ran = false;
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
        auto victim = spawn_in(reask_victim, nullptr, "mreask", VICTIM_PRIO, g_core_mask);
        bool parked = victim.valid() and kos_sem_wait(&g_gates->entering, KOS_TIMEOUT_NONE) == 0;
        bool const killed = victim.valid() and victim.cancel(KOS_CANCEL_KILL, 0) == 0;
        __malloc_unlock(_REENT);
        g_reask_ran = parked and killed and victim.join() == 0;
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
            auto victim = spawn_parked(entry, name, core_mask);
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
        auto holder = spawn_in(reask_holder, nullptr, "mholder", VICTIM_PRIO, core_mask);
        bool const ran = holder.valid() and holder.join() == 0 and g_reask_ran;
        *asked = __atomic_load_n(&g_reask_took, __ATOMIC_RELAXED) == 0;
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
        auto child = spawn_in(getenv_after_kill, nullptr, "menv", VICTIM_PRIO, core_mask);
        bool const ran = child.valid() and kos_sem_wait(&g_gates->entering, KOS_TIMEOUT_NONE) == 0
                         and child.cancel(KOS_CANCEL_KILL, 0) == 0 and child.join() == 0;
        *read = __atomic_load_n(&g_env_after_kill, __ATOMIC_RELAXED) != 0;
        unpin_main(core_mask);
        return ran;
    }
#endif

#if not KICKOS_CHIP_ONE_TASK
    // The peer program run with a role, whose task's exit status is the answer.
    class PeerRun
    {
    public:
        bool start(char const* role, kos_cap_grant const* caps = nullptr, uint8_t cap_count = 0)
        {
            if (kos_program_find(LIBCRACE_PEER, &prog_) != 0)
            {
                return false;
            }
            char const* const argv[] = {"peer", role, nullptr};
            kos_task_params params = {};
            params.argv = argv;
            params.caps = caps;
            params.cap_count = cap_count;
            running_ = kickos::apps::create_peer(prog_, params, &task_) == 0;
            return running_;
        }

        bool running() const
        {
            return running_;
        }

        // The status once the task is dead, or -1000 where it never ran.
        int finish()
        {
            int status = -1000;
            if (running_)
            {
                status = -99;
                while (kickos::apps::wait_dead(task_, &status, 1000u) == -KOS_ETIMEDOUT)
                {
                }
                (void)kos_task_slay(task_, 0);
                running_ = false;
            }
            if (prog_ != KOS_CAP_NONE)
            {
                (void)kos_handle_close(prog_);
                prog_ = KOS_CAP_NONE;
            }
            return status;
        }

    private:
        kos_program_t prog_ = KOS_CAP_NONE;
        kos_task_t task_ = KOS_TASK_NONE;
        bool running_ = false;
    };

    [[maybe_unused]] int run_peer(char const* role)
    {
        PeerRun run;
        if (not run.start(role))
        {
            return -1000;
        }
        return run.finish();
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

#if defined(_NEWLIB_VERSION)
    // Within the allocator's own headers and alignment of the window's end.
    constexpr size_t HEAP_END_SLACK = 64;

    // How far the allocator moved the break, once the heap is exhausted, against the window.
    void report_heap_end()
    {
        char* const brk = static_cast<char*>(_sbrk(0));
        size_t const window = static_cast<size_t>(_kickos_heap_limit - _kickos_heap_start);
        size_t const reached = static_cast<size_t>(brk - _kickos_heap_start);
        if (reached + HEAP_END_SLACK >= window)
        {
            kos::print("libcrace: main's heap ran out at its end\n");
        }
        else
        {
            printf("libcrace: main's heap ran out %u B short of its %u B\n",
                   static_cast<unsigned>(window - reached), static_cast<unsigned>(window));
        }
    }
#endif

    void release_heap(Ballast* head)
    {
        while (head != nullptr)
        {
            Ballast* const next = head->next;
            free(head);
            head = next;
        }
    }

    // One line naming the arm, and main's failure status.
    int refused(char const* arm)
    {
        char line[72];
        ksnprintf(line, sizeof(line), "libcrace: the %s arm was refused\n", arm);
        kos::print(line);
        return 1;
    }
}

int main(int, char**)
{
    printf("libcrace: %d workers x %d rounds\n", WORKERS, ROUNDS);
    gates_seat();
    if (not kill_inside_malloc(malloc_victim, "mvictim"))
    {
        return refused("malloc kill");
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
        return refused("killed waiter's re-ask");
    }
    if (asked)
    {
        kos::print("libcrace: a killed malloc waiter asked again at the kernel and ended there\n");
    }
    else
    {
        kos::print("libcrace: a killed malloc waiter took the lock freed under it\n");
    }
#if KICKOS_CHIP_ONE_TASK
    kos::print("libcrace: skip - a holder killed while running released malloc, and its taker's "
               "task lived: this board holds one task\n");
#else
    int const released = run_peer("release");
    if (released != 0 and released != KOS_EXIT_FAULT)
    {
        return refused("running killed holder");
    }
    if (released == 0)
    {
        kos::print("libcrace: a holder killed while running released malloc, and its taker's "
                   "task lived\n");
    }
    else
    {
        kos::print("libcrace: a holder killed while running died holding malloc, and its taker's "
                   "task ended\n");
    }
#endif
    bool read = false;
    if (not getenv_with_a_kill_pending(&read))
    {
        return refused("getenv kill");
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
    int dead = run_peer("dead_waiting");
    if (dead != 0 and dead != KOS_EXIT_FAULT)
    {
        return refused("dead malloc holder");
    }
    if (dead == KOS_EXIT_FAULT)
    {
        kos::print("libcrace: a malloc lock taken from a dead holder ended its task\n");
    }
    else
    {
        kos::print("libcrace: a malloc lock taken from a dead holder did not end its task\n");
    }
    dead = run_peer("dead_later");
    if (dead != 0 and dead != KOS_EXIT_FAULT)
    {
        return refused("unwatched dead malloc holder");
    }
    if (dead == KOS_EXIT_FAULT)
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
        return refused("printf beside malloc");
    }
    Ballast* const ballast = exhaust_heap();
#if defined(_NEWLIB_VERSION)
    report_heap_end();
#endif
    // Above the racers while the ballast holds the heap: a racer that cannot allocate polls, and a
    // poller above main keeps main from ever releasing it.
    (void)kos_thread_set_priority(WORKER_PRIO + 1);
    kos_cap_t meet_main = KOS_CAP_NONE;
    kos_cap_t meet_peer = KOS_CAP_NONE;
    int started = 0;
#if not KICKOS_CHIP_ONE_TASK
    if (kos_notify_create(&meet_main) != 0 or kos_notify_create(&meet_peer) != 0)
    {
        kos::print("libcrace: no notification for the two tasks' racers to meet on\n");
        return 1;
    }
    constexpr uint8_t PASS_ON = KOS_CAP_TRANSFER;
    kos_cap_grant const peer_meet[2] = {{meet_peer, KOS_CAP_WAIT | PASS_ON},
                                        {meet_main, KOS_CAP_SIGNAL | PASS_ON}};
    PeerRun racers;
    if (racers.start("racers", peer_meet, 2))
    {
        started += WORKERS - MAIN_WORKERS;
    }
    else
    {
        (void)kos_notify(meet_main);
    }
#endif
    uint32_t main_started = 0;
    for (int w = 0; w < MAIN_WORKERS; w++)
    {
        if (spawn_racer(worker, w, meet_main, meet_peer).valid())
        {
            main_started++;
        }
    }
    if (main_started != TASK_RACERS)
    {
        return refused("racer spawn");
    }
    started += static_cast<int>(main_started);
    if (ballast != nullptr and started == WORKERS)
    {
        kos_sem_wait(&g_gates->starved, KOS_TIMEOUT_NONE);
    }
    release_heap(ballast);
    for (uint32_t w = 0; w < main_started; w++)
    {
        kos_sem_wait(&g_gates->done, KOS_TIMEOUT_NONE);
    }
#if not KICKOS_CHIP_ONE_TASK
    if (racers.running())
    {
        if (racers.finish() != 0)
        {
            started = 0;
        }
    }
    (void)kos_handle_close(meet_main);
    (void)kos_handle_close(meet_peer);
#endif
    if (started != WORKERS)
    {
        kos::print("libcrace: spawn refused\n");
        return 1;
    }
    kos::print("libcrace: all workers done\n");
    return 0;
}
