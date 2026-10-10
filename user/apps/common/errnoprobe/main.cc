// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test per-thread errno across blocking switches, slot reuse, IPC fastpath,
// and timer preemption. Use newlib strtol errors (ERANGE/EINVAL), not direct
// assignments, to test libc state selection. Synchronize peers explicitly
// and check required scheduling states. Also test that an exiting thread
// hands the libc scratch it built on demand back to the heap.

#include <kickos/board_config.h>
#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reent.h>
#include <time.h>

namespace
{
    constexpr int WORKERS = 2;

    // Published by one thread and consumed by another, so the ordering is part of the type:
    // a reader that saw `done` must see every field written before it.
    using Flag = kickos::Atomic<unsigned, kickos::Order::ACQUIRE | kickos::Order::RELEASE>;

    constexpr uint8_t CH_FULL = KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

    int g_bad = 0;
    // Sized for the widest line any arm emits: two %lx addresses at the full 16 digits a
    // 64-bit backend can produce, plus the longest verdict. A short buffer truncates the
    // VERDICT, which is what the gate keys on.
    char g_line[192];

    void say(char const* s)
    {
        kos::print(s);
    }

    void fault(char const* s)
    {
        g_bad++;
        kos::print(s);
    }

    // ERANGE for even k, EINVAL for odd, both out of newlib.
    int provoke(int k)
    {
        char* end = nullptr;
        if ((k & 1) == 0)
        {
            (void)strtol("99999999999999999999999999", &end, 10);
        }
        else
        {
            (void)strtol("10", &end, 99);
        }
        return errno;
    }

    int want_for(int k)
    {
        if ((k & 1) == 0)
        {
            return ERANGE;
        }
        return EINVAL;
    }

    // uintptr_t and not unsigned: on a 64-bit target a truncated address can make two
    // distinct slots compare EQUAL, and arm B reads that equality as "the slot was not
    // reused" and suppresses its own fault.
    uintptr_t errno_addr()
    {
        return reinterpret_cast<uintptr_t>(&errno);
    }

#if defined(__x86_64__) && !KICKOS_ARCH_SIM
    // --- arm F: libc on a core's first thread, before any switch ------------------------
    // Root is a core's first thread, entered by arch_start with no switch, so only the FS base
    // that path wrote can hand libc this thread's state. The app's constructors run on root
    // before the init it then runs switches anything in, so the arm is one: strtok and strtol
    // out of newlib, then which core entered root. Its line waits for main to print it.
    // Not excluded: an interrupt switching root out and back in before the reads.
    char g_first_line[96];

    __attribute__((constructor)) void arm_first_thread()
    {
        char buf[] = "kick os";
        char const* const t0 = strtok(buf, " ");
        char const* const t1 = strtok(nullptr, " ");
        errno = 0;
        char* end = nullptr;
        (void)strtol("99999999999999999999999999", &end, 10);
        int const e = errno;
        struct _reent* const r = __getreent();
        uintptr_t fs0 = 0;
        __asm__ volatile("movq %%fs:0, %0" : "=r"(fs0));
        int const core = kos_core_current();

        char const* verdict = "ok";
        if (t0 == nullptr or t1 == nullptr or strcmp(t0, "kick") != 0 or strcmp(t1, "os") != 0)
        {
            verdict = "STRTOK LOST ITS PLACE";
        }
        else if (e != ERANGE)
        {
            verdict = "STRTOL DID NOT SET ERRNO";
        }
        else if (reinterpret_cast<uintptr_t>(r) != fs0)
        {
            verdict = "__getreent IS NOT THE FS CELL'S WORD";
        }
        else if (r == _GLOBAL_REENT)
        {
            verdict = "LIBC RAN ON THE PROCESS-WIDE STATE";
        }
        else if (core < 0)
        {
            verdict = "THE CORE IS UNREADABLE";
        }
        if (verdict[0] != 'o')
        {
            g_bad++;
        }
        ksnprintf(g_first_line, sizeof(g_first_line), "[errnoprobe] F first core %d %s\n", core,
                  verdict);
    }
#endif

    // --- arm A: a cooperative round trip that crossed a peer's write -------------------

    struct Report
    {
        uintptr_t addr; // where libc resolves this thread's errno
        int provoked;   // errno straight after the call that set it
        int after_trip; // errno after the peer has provoked its own
        unsigned trips; // scheduler round trips waited out
        Flag done;
    };

    Report g_report[WORKERS] = {};
    Flag g_provoked[WORKERS] = {};

    void worker(void* arg)
    {
        int const k = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        g_report[k].addr = errno_addr();
        g_report[k].provoked = provoke(k);
        g_provoked[k] = 1;
        // WAIT FOR THE PEER'S WRITE, not just for time to pass: the read below has to happen
        // after another thread has put a different value in libc's state word, or an
        // implementation that seats the pointer once at thread entry would pass.
        unsigned trips = 0;
        while (true)
        {
            kos::sleep_ns(20000000ull);
            trips++;
            unsigned ready = 0;
            for (int i = 0; i < WORKERS; i++)
            {
                ready += g_provoked[i].load();
            }
            if (ready == WORKERS or trips > 200)
            {
                break;
            }
        }
        g_report[k].after_trip = errno;
        g_report[k].trips = trips;
        g_report[k].done = 1;
        // Returning frees the slot, which is what arm B goes on to reuse.
    }

    void arm_cooperative()
    {
        uintptr_t const main_addr = errno_addr();
        int const main_provoked = provoke(0); // main takes ERANGE, like worker 0

        kos::thread::Handle w[WORKERS];
        for (int k = 0; k < WORKERS; k++)
        {
            w[k] = kos::thread::create(worker, reinterpret_cast<void*>(static_cast<uintptr_t>(k)),
                                       "errw", 10);
        }

        for (int spin = 0; spin < 300; spin++)
        {
            int ready = 0;
            for (int k = 0; k < WORKERS; k++)
            {
                ready += static_cast<int>(g_report[k].done.load());
            }
            if (ready == WORKERS)
            {
                break;
            }
            kos::sleep_ns(20000000ull);
        }

        for (int k = 0; k < WORKERS; k++)
        {
            int const want = want_for(k);
            char const* verdict = "ok";
            if (g_report[k].done.load() == 0)
            {
                verdict = "NEVER RAN";
                g_bad++;
            }
            else if (g_report[k].provoked != want)
            {
                verdict = "LIBC SET THE WRONG VALUE";
                g_bad++;
            }
            else if (g_report[k].after_trip != want)
            {
                verdict = "LOST ITS OWN ERRNO ACROSS A SWITCH";
                g_bad++;
            }
            else if (g_report[k].addr == main_addr)
            {
                verdict = "SHARES ROOT'S STATE";
                g_bad++;
            }
            ksnprintf(g_line, sizeof(g_line),
                      "[errnoprobe] A w%d at %lx provoked %d after %d trips %u %s\n", k,
                      static_cast<unsigned long>(g_report[k].addr), g_report[k].provoked,
                      g_report[k].after_trip,
                      g_report[k].trips, verdict);
            say(g_line);
        }

        if (g_report[0].addr == g_report[1].addr)
        {
            fault("[errnoprobe] A w0 and w1 SHARE ONE STATE\n");
        }
        if (g_report[0].after_trip == g_report[1].after_trip)
        {
            fault("[errnoprobe] A BOTH WORKERS READ THE SAME ERRNO\n");
        }
        // Main provoked ERANGE before either worker ran and worker 1 then provoked EINVAL, so
        // a shared state hands main the peer's value.
        int const main_now = errno;
        if (main_provoked != ERANGE or main_now != ERANGE)
        {
            g_bad++;
        }
        ksnprintf(g_line, sizeof(g_line),
                  "[errnoprobe] A main at %lx provoked %d now %d (want %d)\n",
                  static_cast<unsigned long>(main_addr), main_provoked, main_now, ERANGE);
        say(g_line);

        for (int k = 0; k < WORKERS; k++)
        {
            (void)w[k].join();
        }
    }

    // --- arm B: a slot handed on to a new thread ---------------------------------------
    //
    // The struct _reent array is indexed by THREAD SLOT, not by thread, so a slot's state
    // outlives its occupant. ThreadPool::alloc hands out the lowest EXITED slot, so joining
    // the first worker makes the second one land on the same index by construction; the arm
    // asserts they did, or it would pass vacuously on a pool that never reused anything.

    struct Reuse
    {
        uintptr_t addr;
        int first;    // errno BEFORE this thread has called anything that sets it
        int provoked; // and after its own call
        Flag done;
    };

    Reuse g_reuse[2] = {};

    void reuse_worker(void* arg)
    {
        int const k = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        g_reuse[k].addr = errno_addr();
        g_reuse[k].first = errno;
        g_reuse[k].provoked = provoke(k);
        g_reuse[k].done = 1;
    }

    void arm_slot_reuse()
    {
        for (int k = 0; k < 2; k++)
        {
            kos::thread::Handle const h = kos::thread::create(
                reuse_worker, reinterpret_cast<void*>(static_cast<uintptr_t>(k)), "reus", 10);
            if (not h.valid())
            {
                fault("[errnoprobe] B SPAWN FAILED\n");
                return;
            }
            // Joining is what frees the slot, and freeing it is the whole arm: without the
            // join the second thread takes a fresh slot and witnesses nothing.
            (void)h.join();
        }

        for (int k = 0; k < 2; k++)
        {
            char const* verdict = "ok";
            if (g_reuse[k].done.load() == 0)
            {
                verdict = "NEVER RAN";
                g_bad++;
            }
            else if (g_reuse[k].first != 0)
            {
                verdict = "STARTED ON THE PRIOR OCCUPANT'S STATE";
                g_bad++;
            }
            else if (g_reuse[k].provoked != want_for(k))
            {
                verdict = "LIBC SET THE WRONG VALUE";
                g_bad++;
            }
            ksnprintf(g_line, sizeof(g_line),
                      "[errnoprobe] B t%d at %lx first %d provoked %d %s\n", k,
                      static_cast<unsigned long>(g_reuse[k].addr), g_reuse[k].first,
                      g_reuse[k].provoked, verdict);
            say(g_line);
        }
        if (g_reuse[0].addr != g_reuse[1].addr)
        {
            fault("[errnoprobe] B THE SLOT WAS NOT REUSED, so this arm proves nothing\n");
        }
    }

    // --- arm C: the IPC fastpath --------------------------------------------------------
    //
    // kos_call swaps registers inside the trap handler and reaches the scheduler through
    // sched::switch_prepare, never through the ordinary switch_to. Every condition the
    // kernel fastpath requires is met here BY CONSTRUCTION: both lengths are inside
    // KOS_CALL_REG_BYTES, the caller's priority is below the server's, and the server is
    // parked in an info-bearing recv before the caller exists (it posts `ready` at a
    // priority the waiter cannot preempt, so it reaches the park before main runs again).

    struct Fast
    {
        uintptr_t server_addr;
        uintptr_t client_addr;
        int server_provoked;
        int server_in_dispatch; // errno as the server sees it on the fastpath resume
        int client_provoked;
        int client_after;
        int32_t call_rc;
        int32_t recv_rc;
        unsigned had_reply_cap;
        Flag server_done;
        Flag client_done;
    };

    Fast g_fast = {};

    constexpr kos_cap_t FAST_EP = 1;
    constexpr kos_cap_t FAST_READY = 2;
    constexpr kos_cap_t FAST_SRV_DONE = 3;
    constexpr kos_cap_t FAST_CLI_DONE = 2;

    void fast_server(void*)
    {
        g_fast.server_addr = errno_addr();
        g_fast.server_provoked = provoke(1); // EINVAL
        unsigned char buf[KOS_CALL_REG_BYTES];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        kos_sem_post(FAST_READY);
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, FAST_EP, 0, KOS_TIMEOUT_NONE);
        g_fast.recv_rc = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        info = opts.info;
        // The first thing this thread does after the fastpath put it back on the CPU.
        g_fast.server_in_dispatch = errno;
        if (info.reply_cap != KOS_CAP_NONE)
        {
            g_fast.had_reply_cap = 1;
            (void)kos_reply(info.reply_cap, buf, 4);
        }
        g_fast.server_done = 1;
        kos_sem_post(FAST_SRV_DONE);
    }

    void fast_client(void*)
    {
        g_fast.client_addr = errno_addr();
        g_fast.client_provoked = provoke(0); // ERANGE
        unsigned char buf[KOS_CALL_REG_BYTES] = {1, 2, 3, 4};
        g_fast.call_rc = kos_call(FAST_EP, buf, 4, 4);
        g_fast.client_after = errno;
        g_fast.client_done = 1;
        kos_sem_post(FAST_CLI_DONE);
    }

    void arm_fastpath()
    {
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            fault("[errnoprobe] C NO ENDPOINT\n");
            return;
        }
        kos::Semaphore ready(0);
        kos::Semaphore done(0);
        kos_cap_grant scaps[] = {{ep, KOS_CAP_WAIT}, {ready.id(), CH_FULL}, {done.id(), CH_FULL}};
        kos_cap_grant ccaps[] = {{ep, KOS_CAP_SIGNAL}, {done.id(), CH_FULL}};

        // Server ABOVE the caller: the fastpath refuses a call that would need priority
        // donation, so a caller that outranked the server would silently take the generic
        // path and this arm would witness the wrong one.
        kos::thread::Handle const sv =
            kos::thread::create_caps(fast_server, nullptr, "fpS", 12, scaps, 3);
        if (not sv.valid())
        {
            fault("[errnoprobe] C SERVER SPAWN FAILED\n");
            return;
        }
        // Returns only once the server has posted, and the server posts from a priority main
        // cannot preempt, so by the time main runs again the server is parked in its recv.
        ready.wait();

        kos::thread::Handle const cl =
            kos::thread::create_caps(fast_client, nullptr, "fpC", 10, ccaps, 2);
        if (not cl.valid())
        {
            fault("[errnoprobe] C CLIENT SPAWN FAILED\n");
            return;
        }
        done.wait();
        done.wait();

        char const* verdict = "ok";
        if (g_fast.server_done.load() == 0 or g_fast.client_done.load() == 0)
        {
            verdict = "AN END NEVER RAN";
            g_bad++;
        }
        else if (g_fast.had_reply_cap == 0)
        {
            verdict = "NO REPLY CAP, so this was not a call at all";
            g_bad++;
        }
        else if (g_fast.server_provoked != EINVAL or g_fast.client_provoked != ERANGE)
        {
            verdict = "LIBC SET THE WRONG VALUE";
            g_bad++;
        }
        else if (g_fast.server_in_dispatch != EINVAL)
        {
            verdict = "SERVER DISPATCHED ON THE CALLER'S ERRNO";
            g_bad++;
        }
        else if (g_fast.client_after != ERANGE)
        {
            verdict = "CALLER LOST ITS ERRNO ACROSS THE CALL";
            g_bad++;
        }
        else if (g_fast.server_addr == g_fast.client_addr)
        {
            verdict = "BOTH ENDS SHARE ONE STATE";
            g_bad++;
        }
        ksnprintf(g_line, sizeof(g_line),
                  "[errnoprobe] C srv %lx in-dispatch %d cli %lx after %d rc %d/%d %s\n",
                  static_cast<unsigned long>(g_fast.server_addr), g_fast.server_in_dispatch,
                  static_cast<unsigned long>(g_fast.client_addr),
                  g_fast.client_after, static_cast<int>(g_fast.recv_rc),
                  static_cast<int>(g_fast.call_rc), verdict);
        say(g_line);

        (void)sv.join();
        (void)cl.join();
        kos_handle_close(ep);
    }

    // --- arm D: an involuntary preemption ------------------------------------------------
    //
    // The low thread makes NO syscall between publishing `spinning` and reading its errno
    // back, so the only thing that can take the CPU away from it is the timer interrupt that
    // wakes the high one. The high thread records whether the low one was actually spinning
    // when it got there, which is what stops this arm passing on a run where the two never
    // overlapped.

    struct Preempt
    {
        uintptr_t lo_addr;
        uintptr_t hi_addr;
        int lo_provoked;
        int lo_after;
        int hi_provoked;
        unsigned saw_lo_spinning;
        unsigned spin_exhausted;
        unsigned wakes;
        Flag spinning;
        Flag hi_published;
        Flag lo_done;
    };

    Preempt g_pre = {};

    constexpr unsigned long long PREEMPT_SLEEP_NS = 10000000ull;

    // The guest clock tracks HOST time (QEMU runs without icount), so a contended host can spend
    // a whole sleep with the guest barely executing and the low thread not yet spinning. Every
    // wake is a timer interrupt that takes the CPU off the low thread, so whichever wake sees
    // `spinning` witnesses the preemption.
    constexpr unsigned PREEMPT_WAKES = 50;

    // Must outlast PREEMPT_WAKES sleeps. The loop reaches at most 2.0e6 iterations per 10 ms
    // on the QEMU backends this app boots on, so a 50-wake budget costs under 2e8 even at twice
    // that rate, leaving this bound 5x spare. An exhausted spin is reported as a failure rather
    // than passing.
    constexpr unsigned long PREEMPT_SPIN_LIMIT = 1000000000ul;

    constexpr kos_cap_t PRE_DONE = 1;

    void preempt_high(void*)
    {
        g_pre.hi_addr = errno_addr();
        while (g_pre.wakes < PREEMPT_WAKES)
        {
            // parks; the wake is a timer interrupt, not a peer's post
            kos_sleep_ns(PREEMPT_SLEEP_NS);
            g_pre.wakes++;
            if (g_pre.spinning.load() != 0)
            {
                g_pre.saw_lo_spinning = 1;
                break;
            }
        }
        g_pre.hi_provoked = provoke(1); // EINVAL, written while the low thread is mid-spin
        g_pre.hi_published = 1;
        kos_sem_post(PRE_DONE);
    }

    void preempt_low(void*)
    {
        g_pre.lo_addr = errno_addr();
        g_pre.lo_provoked = provoke(0); // ERANGE
        g_pre.spinning = 1;
        unsigned long i = 0;
        while (g_pre.hi_published.load() == 0)
        {
            i++;
            if (i >= PREEMPT_SPIN_LIMIT)
            {
                g_pre.spin_exhausted = 1;
                break;
            }
        }
        // No syscall has run on this thread since `spinning` was published, so whatever took
        // the CPU away in between did so without being asked.
        g_pre.lo_after = errno;
        g_pre.lo_done = 1;
        kos_sem_post(PRE_DONE);
    }

    void arm_preemption()
    {
        kos::Semaphore done(0);
        kos_cap_grant caps[] = {{done.id(), CH_FULL}};
        kos::thread::Handle const hi =
            kos::thread::create_caps(preempt_high, nullptr, "prH", 20, caps, 1);
        kos::thread::Handle const lo =
            kos::thread::create_caps(preempt_low, nullptr, "prL", 8, caps, 1);
        if (not hi.valid() or not lo.valid())
        {
            fault("[errnoprobe] D SPAWN FAILED\n");
            return;
        }
        // Neither spawn preempted main, so the high thread runs first and is parked in its
        // sleep before the low one starts spinning.
        done.wait();
        done.wait();

        char const* verdict = "ok";
        if (g_pre.lo_done.load() == 0)
        {
            verdict = "THE LOW THREAD NEVER FINISHED";
            g_bad++;
        }
        else if (g_pre.spin_exhausted != 0)
        {
            verdict = "SPIN EXHAUSTED, so nothing preempted it";
            g_bad++;
        }
        else if (g_pre.saw_lo_spinning == 0)
        {
            verdict = "THE TWO NEVER OVERLAPPED, so this arm proves nothing";
            g_bad++;
        }
        else if (g_pre.lo_provoked != ERANGE or g_pre.hi_provoked != EINVAL)
        {
            verdict = "LIBC SET THE WRONG VALUE";
            g_bad++;
        }
        else if (g_pre.lo_after != ERANGE)
        {
            verdict = "PREEMPTED THREAD CAME BACK ON THE PEER'S ERRNO";
            g_bad++;
        }
        else if (g_pre.lo_addr == g_pre.hi_addr)
        {
            verdict = "BOTH SHARE ONE STATE";
            g_bad++;
        }
        ksnprintf(g_line, sizeof(g_line),
                  "[errnoprobe] D lo %lx after %d hi %lx set %d overlapped %u wakes %u %s\n",
                  static_cast<unsigned long>(g_pre.lo_addr), g_pre.lo_after,
                  static_cast<unsigned long>(g_pre.hi_addr), g_pre.hi_provoked,
                  g_pre.saw_lo_spinning, g_pre.wakes, verdict);
        say(g_line);

        (void)hi.join();
        (void)lo.join();
    }

    // --- arm E: libc scratch handed back at thread exit ----------------------------------
    //
    // Where newlib builds its per-thread scratch on the heap on first use, the slot's next
    // prime overwrites the pointers, so whatever an exiting thread does not free itself is
    // lost. Every round starts a thread that touches each lazily built member it can reach
    // through ISO C within the board's flash and then ends, alternately by returning and by
    // exiting; the heap's in-use bytes must not move across the rounds. Main touches none of
    // that state meanwhile, and nothing else in this image allocates.

    constexpr unsigned RECLAIM_ROUNDS = 8;

    Flag g_took = {}; // in-use bytes the last worker added
    Flag g_reclaim_ran = {};
    char g_tok[] = "a b";

    unsigned heap_in_use()
    {
        return static_cast<unsigned>(mallinfo().uordblks);
    }

    void reclaim_worker(void* arg)
    {
        unsigned const before = heap_in_use();
        g_tok[1] = ' '; // strtok overwrote it last round
        (void)strtok(g_tok, " ");
        (void)rand();
#if !KICKOS_DIAG_TERSE
        // A board short enough of flash to take the terse diagnostic column has none for these:
        // full newlib's localtime and asctime bring its formatted I/O along, and nano's strtod
        // its floating-point conversion.
        time_t const t = 0;
        (void)asctime(localtime(&t));
#if defined(_REENT_SMALL) && KICKOS_USER_HEAP_SIZE >= 2048
        // Only here: the full profile keeps this state inline but still takes its lists from
        // the heap, and nothing hands those back. With the calls above this conversion takes
        // 1,628 B of Arm nano heap, which a smaller carve refuses with an assertion.
        (void)strtod("1.25e300", nullptr);
#endif
#endif
        g_took = heap_in_use() - before;
        g_reclaim_ran = g_reclaim_ran.load() + 1u;
        if (arg != nullptr)
        {
            kos::exit(0);
        }
    }

    bool reclaim_round(unsigned r)
    {
        void* arg = nullptr;
        if ((r & 1u) != 0)
        {
            arg = g_tok;
        }
        kos::thread::Handle const h = kos::thread::create(reclaim_worker, arg, "recl", 10);
        if (not h.valid())
        {
            return false;
        }
        (void)h.join();
        return true;
    }

    void arm_reclaim()
    {
        // The first round may take one-time process-wide state (time zone, stdio) that is not
        // any thread's to return.
        if (not reclaim_round(0))
        {
            fault("[errnoprobe] E SPAWN FAILED\n");
            return;
        }
        unsigned const base = heap_in_use();
        for (unsigned r = 1; r <= RECLAIM_ROUNDS; r++)
        {
            if (not reclaim_round(r))
            {
                fault("[errnoprobe] E SPAWN FAILED\n");
                return;
            }
        }
        unsigned const now = heap_in_use();

        char const* verdict = "ok";
        if (g_reclaim_ran.load() != RECLAIM_ROUNDS + 1u)
        {
            verdict = "A WORKER NEVER RAN";
            g_bad++;
        }
        else if (now > base)
        {
            verdict = "EXITED THREADS LEAK THEIR LIBC SCRATCH";
            g_bad++;
        }
#ifdef _REENT_SMALL
        else if (g_took.load() == 0)
        {
            verdict = "THE WORKER BUILT NO SCRATCH, so this arm proves nothing";
            g_bad++;
        }
#endif
        ksnprintf(g_line, sizeof(g_line),
                  "[errnoprobe] E rounds %u per-thread %u in-use %u -> %u %s\n",
                  RECLAIM_ROUNDS, g_took.load(), base, now, verdict);
        say(g_line);
    }
}

int main(int, char**)
{
    kos::print("[errnoprobe] start\n");
#if defined(__x86_64__) && !KICKOS_ARCH_SIM
    say(g_first_line);
#endif

    arm_cooperative();
    arm_slot_reuse();
    arm_fastpath();
    arm_preemption();
    arm_reclaim();

    if (g_bad == 0)
    {
        kos::print("[errnoprobe] PASS\n");
    }
    else
    {
        kos::print("[errnoprobe] FAIL\n");
    }
    // Main's return ends its task, which ends the system and the qemu run.
    return 0;
}
