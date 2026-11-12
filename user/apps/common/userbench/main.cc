// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Semaphore ping-pong, IPC and wake microbenchmarks, every row timed from user space on a plain
// kernel.

#include <kickos/board_config.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/irq_free.h>
#include <kickos/libc/fmt.h>

#include <kickos/apps/peer_run.h>

#include "callreply.h"
#include "clock.h"
#include "walk.h"

#ifndef KICKOS_KERNEL_CORES
#define KICKOS_KERNEL_CORES 1
#endif

#if defined(__x86_64__) && !KICKOS_ARCH_SIM
#define BENCH_VEC_DIRTY 1
// Writes all sixteen YMM registers whole, in assembly of its own: the compiler is not told, and
// tests/static/check_x86_64_no_vector.sh admits it by name.
extern "C" __attribute__((visibility("hidden"))) void bench_vec_dirty(void);
__asm__(".pushsection .text, \"ax\", @progbits\n"
        "    .balign 16\n"
        "    .globl bench_vec_dirty\n"
        "    .hidden bench_vec_dirty\n"
        "    .type bench_vec_dirty, @function\n"
        "bench_vec_dirty:\n"
        "    vpcmpeqd %ymm0, %ymm0, %ymm0\n    vpcmpeqd %ymm1, %ymm1, %ymm1\n"
        "    vpcmpeqd %ymm2, %ymm2, %ymm2\n    vpcmpeqd %ymm3, %ymm3, %ymm3\n"
        "    vpcmpeqd %ymm4, %ymm4, %ymm4\n    vpcmpeqd %ymm5, %ymm5, %ymm5\n"
        "    vpcmpeqd %ymm6, %ymm6, %ymm6\n    vpcmpeqd %ymm7, %ymm7, %ymm7\n"
        "    vpcmpeqd %ymm8, %ymm8, %ymm8\n    vpcmpeqd %ymm9, %ymm9, %ymm9\n"
        "    vpcmpeqd %ymm10, %ymm10, %ymm10\n    vpcmpeqd %ymm11, %ymm11, %ymm11\n"
        "    vpcmpeqd %ymm12, %ymm12, %ymm12\n    vpcmpeqd %ymm13, %ymm13, %ymm13\n"
        "    vpcmpeqd %ymm14, %ymm14, %ymm14\n    vpcmpeqd %ymm15, %ymm15, %ymm15\n"
        "    ret\n"
        ".popsection\n");
#elif defined(__aarch64__)
#define BENCH_VEC_DIRTY 1
// Writes all thirty-two SIMD registers whole, in assembly the compiler is not told of.
extern "C" __attribute__((visibility("hidden"))) void bench_vec_dirty(void);
#define BENCH_VEC_MOVI(a, b) "    movi v" #a ".16b, #0xff\n    movi v" #b ".16b, #0xff\n"
__asm__(".pushsection .text, \"ax\", %progbits\n"
        "    .balign 16\n"
        "    .globl bench_vec_dirty\n"
        "    .hidden bench_vec_dirty\n"
        "    .type bench_vec_dirty, %function\n"
        "bench_vec_dirty:\n"
        BENCH_VEC_MOVI(0, 1) BENCH_VEC_MOVI(2, 3) BENCH_VEC_MOVI(4, 5) BENCH_VEC_MOVI(6, 7)
        BENCH_VEC_MOVI(8, 9) BENCH_VEC_MOVI(10, 11) BENCH_VEC_MOVI(12, 13) BENCH_VEC_MOVI(14, 15)
        BENCH_VEC_MOVI(16, 17) BENCH_VEC_MOVI(18, 19) BENCH_VEC_MOVI(20, 21) BENCH_VEC_MOVI(22, 23)
        BENCH_VEC_MOVI(24, 25) BENCH_VEC_MOVI(26, 27) BENCH_VEC_MOVI(28, 29) BENCH_VEC_MOVI(30, 31)
        "    ret\n"
        ".popsection\n");
#else
#define BENCH_VEC_DIRTY 0
#endif

#if defined(__XTENSA__)
#define BENCH_FLASH_TEXT 1
// Eight functions one cache way apart (16 KiB, half the 32 KiB two-way cache, ESP32 TRM v5.8
// 3.3.4): all eight share a set, so calling them in turn misses on six of eight calls at least,
// whatever the undocumented replacement.
#define BENCH_FLASH_ALIAS(n)                                                                      \
    "    .balign 16384\n"                                                                        \
    "    .globl bench_flash_alias" #n "\n"                                                       \
    "    .type bench_flash_alias" #n ", @function\n"                                             \
    "bench_flash_alias" #n ":\n"                                                                 \
    "    entry a1, 16\n"                                                                         \
    "    retw.n\n"
extern "C" void bench_flash_alias0(void);
extern "C" void bench_flash_alias1(void);
extern "C" void bench_flash_alias2(void);
extern "C" void bench_flash_alias3(void);
extern "C" void bench_flash_alias4(void);
extern "C" void bench_flash_alias5(void);
extern "C" void bench_flash_alias6(void);
extern "C" void bench_flash_alias7(void);
__asm__(".pushsection .text.bench_flash_alias, \"ax\", @progbits\n"
        BENCH_FLASH_ALIAS(0) BENCH_FLASH_ALIAS(1) BENCH_FLASH_ALIAS(2) BENCH_FLASH_ALIAS(3)
        BENCH_FLASH_ALIAS(4) BENCH_FLASH_ALIAS(5) BENCH_FLASH_ALIAS(6) BENCH_FLASH_ALIAS(7)
        ".popsection\n");
#else
#define BENCH_FLASH_TEXT 0
#endif

namespace
{
    using namespace bench;
    using kickos::Atomic;
    using kickos::Order;

    // Rounds per throughput window: a few seconds on a slow M0.
    constexpr uint32_t ROUNDS_PER_REPORT = 20000;
    // Bounded so main returns, which ends the run.
    constexpr unsigned THROUGHPUT_REPORTS = 3;

    // One buffer for every per-sample row, each run after the last has printed; a mean keeps
    // no sample.
    constexpr uint32_t SAMPLES = 1000;
    constexpr uint32_t samples_kept()
    {
        if (FINE_CLOCK)
        {
            return SAMPLES;
        }
        return 1;
    }
    uint32_t g_samples[samples_kept()];
    constexpr uint32_t TARE_SAMPLES = 64;

    // The players' handoff pair, the reporter-wake gate player_b posts, and the resume the
    // reporter posts as the next window opens.
    kos_sem_t g_a = KOS_SEM_INIT(0);
    kos_sem_t g_b = KOS_SEM_INIT(0);
    kos_sem_t g_gate = KOS_SEM_INIT(0);
    kos_sem_t g_resume = KOS_SEM_INIT(0);
    Atomic<uint32_t, Order::RELAXED> g_rounds{0};
    // Set for the dirty-vector window: each player writes every vector register before each
    // switch, so each restore loads them rather than their initial state.
    Atomic<uint32_t, Order::RELAXED> g_vec_dirty{0};

    void vec_dirty()
    {
#if BENCH_VEC_DIRTY
        if (g_vec_dirty != 0)
        {
            bench_vec_dirty();
        }
#endif
    }

    void player_a(void*)
    {
        while (true)
        {
            vec_dirty();
            kos_sem_wait(&g_a, KOS_TIMEOUT_NONE);
            vec_dirty();
            kos_sem_post(&g_b);
        }
    }

    // Parks for the whole report, with player_a parked on A behind it.
    void player_b(void*)
    {
        // The reporter opens every burst through resume, the first included.
        kos_sem_wait(&g_resume, KOS_TIMEOUT_NONE);
        while (true)
        {
            vec_dirty();
            kos_sem_post(&g_a);
            vec_dirty();
            kos_sem_wait(&g_b, KOS_TIMEOUT_NONE);
            uint32_t const round = g_rounds + 1;
            g_rounds = round;
            if ((round % ROUNDS_PER_REPORT) == 0)
            {
                kos_sem_post(&g_gate);
                kos_sem_wait(&g_resume, KOS_TIMEOUT_NONE);
            }
        }
    }

    struct Window
    {
        uint64_t switches;
        uint64_t d_ns;
    };

    // One burst of the players, alone: they are parked from the gate until this release.
    Window window()
    {
        uint32_t const prev_rounds = g_rounds;
        uint64_t const prev_ns = kos::clock_now();
        kos_sem_post(&g_resume);
        kos_sem_wait(&g_gate, KOS_TIMEOUT_NONE);
        uint64_t const now_ns = kos::clock_now();
        uint32_t const rounds = g_rounds;
        Window w = {static_cast<uint64_t>(rounds - prev_rounds) * 2ull, now_ns - prev_ns};
        return w;
    }

    uint32_t ns_per_switch(Window const& w)
    {
        if (w.switches == 0)
        {
            return 0;
        }
        return static_cast<uint32_t>(w.d_ns / w.switches);
    }

    // Returns the last window's ns per switch.
    uint32_t reporter_loop()
    {
        uint32_t last = 0;
        for (unsigned rep = 0; rep < THROUGHPUT_REPORTS; rep++)
        {
            Window const w = window();
            uint64_t const switches = w.switches;
            uint64_t const d_ns = w.d_ns;

            uint32_t sw_per_s = 0;
            uint32_t ns_per_sw = 0;
            if (d_ns != 0 and switches != 0)
            {
                sw_per_s = static_cast<uint32_t>(switches * 1000000000ull / d_ns);
                ns_per_sw = ns_per_switch(w);
            }
            last = ns_per_sw;

            // Every %u argument is cast to `unsigned`: uint32_t is `unsigned long` on the newlib
            // targets and `unsigned` elsewhere, and `unsigned` is 32-bit everywhere KickOS runs.
            char s[160];
            ksnprintf(s, sizeof(s),
                      "  throughput: %u ctx-sw/s  (%u ns/sw avg over %u switches / %u ms)\n",
                      static_cast<unsigned>(sw_per_s), static_cast<unsigned>(ns_per_sw),
                      static_cast<unsigned>(switches),
                      static_cast<unsigned>(d_ns / 1000000ull));
            kos::print(s);
        }
        return last;
    }

    // The same ping-pong with the players' vector state dirty, against the last clean window.
    void switch_vec(uint32_t clean)
    {
#if BENCH_VEC_DIRTY
        g_vec_dirty = 1;
        Window const w = window();
        g_vec_dirty = 0;
        uint32_t const dirty = ns_per_switch(w);
        char s[160];
        ksnprintf(s, sizeof(s), "  switch-vec: %d ns/sw  (dirty %u - clean %u ns/sw, %u switches)\n",
                  static_cast<int>(static_cast<int32_t>(dirty) - static_cast<int32_t>(clean)),
                  static_cast<unsigned>(dirty), static_cast<unsigned>(clean),
                  static_cast<unsigned>(w.switches));
        kos::print(s);
#else
        (void)clean;
#endif
    }

    // --- the end-to-end span -------------------------------------------------------------
    // main raises a line whose notification wakes a waiter above it on its own core: the span
    // is from main's stamp before the raise to the waiter's stamp at its wake.
    constexpr int E2E_LINE = KICKOS_IRQ_FREE_BASE + 0;
    // waiter: the object the line signals, WAIT only
    constexpr int CH_E2E_NOTE = KOS_SPAWN_DELEGATED_CAP0;
    // Main attaches the line with an unbadged copy, so it raises bit 0.
    constexpr uint32_t E2E_LINE_BIT = 1u << 0;
    constexpr uint8_t E2E_WAITER_PRIO = 15;
    // A raise whose wake has not landed by then ends the row.
    constexpr uint64_t E2E_STALL_NS = 1000000000ull;
#if KICKOS_KERNEL_CORES > 1
    constexpr uint32_t E2E_CORES = 1u << 0;
#else
    constexpr uint32_t E2E_CORES = 0;
#endif

    kos_sem_t g_e2e_ready = KOS_SEM_INIT(0);
    uint64_t volatile g_e2e_t0 = 0;
    Series g_e2e;
    // Bumped by the waiter after its sample.
    Atomic<uint32_t, Order::RELAXED> g_e2e_done{0};
    Atomic<int32_t, Order::RELAXED> g_e2e_rc{0};

    void e2e_waiter(void*)
    {
        g_e2e_rc = kos_notify_bind(CH_E2E_NOTE);
        kos_sem_post(&g_e2e_ready);
        if (g_e2e_rc != 0)
        {
            return;
        }
        while (true)
        {
            if (kos_notify_wait(CH_E2E_NOTE, E2E_LINE_BIT, KOS_TIMEOUT_NONE, nullptr) != 0)
            {
                break; // cancelled: nothing left to wake for
            }
            uint64_t const t1 = stamp();
            g_e2e.add(span_ns(g_e2e_t0, t1));
            g_e2e_done = g_e2e_done + 1;
        }
    }

    bool e2e_await(uint32_t before)
    {
        uint64_t const t0 = kos_clock_now();
        while (g_e2e_done == before)
        {
            if (kos_clock_now() - t0 > E2E_STALL_NS)
            {
                return false;
            }
            kos_yield();
        }
        return true;
    }

    void e2e_skip(char const* why, int rc)
    {
        char s[128];
        ksnprintf(s, sizeof(s), "  e2e-local: SKIP (%s, rc=%d)\n", why, rc);
        kos::print(s);
    }

    void e2e_local(uint32_t tare)
    {
        kos_thread_t const self = kos_thread_self();
        if (KICKOS_KERNEL_CORES > 1)
        {
            // A line is claimed by a thread pinned where it runs, and the waiter is pinned there.
            (void)kos_thread_affinity(self, E2E_CORES);
        }
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        int rc = kos_irq_claim(E2E_LINE, KOS_IRQ_EDGE, &irq);
        if (rc != 0)
        {
            e2e_skip("line claim refused", rc);
        }
        else
        {
            rc = kos_notify_create(&note);
            if (rc == 0)
            {
                rc = kos_irq_bind_notify(irq, note);
            }
            if (rc != 0)
            {
                e2e_skip("the line could not be attached to a notification", rc);
            }
        }
        kos::thread::Handle w;
        if (rc == 0)
        {
            // The claim leaves the line masked; the waiter's waits rearm it after.
            (void)kos_irq_ack(irq);
            kos_cap_grant caps[] = {{note, KOS_CAP_WAIT}};
            w = kos::thread::create_caps(e2e_waiter, nullptr, "e2ewait", E2E_WAITER_PRIO, caps, 1,
                                         KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0, E2E_CORES);
            if (not w.valid())
            {
                rc = -KOS_ENOMEM;
                e2e_skip("thread pool too small for the waiter", rc);
            }
        }
        if (rc == 0)
        {
            kos_sem_wait(&g_e2e_ready, KOS_TIMEOUT_NONE);
            rc = g_e2e_rc;
            if (rc != 0)
            {
                e2e_skip("the waiter could not bind the notification", rc);
            }
        }
        if (rc == 0)
        {
            g_e2e = series(g_samples, samples_kept());
            uint32_t i = 0;
            for (; i < SAMPLES; i++)
            {
                uint32_t const before = g_e2e_done;
                g_e2e_t0 = stamp();
                rc = kos_irq_raise(irq);
                if (rc != 0 or not e2e_await(before))
                {
                    break;
                }
            }
            if (rc != 0)
            {
                e2e_skip("raise refused", rc);
            }
            else if (i != SAMPLES)
            {
                char s[96];
                ksnprintf(s, sizeof(s), "  e2e-local: FAIL (no wake after %u samples)\n",
                          static_cast<unsigned>(i));
                kos::print(s);
            }
            else
            {
                char tail[48];
                ksnprintf(tail, sizeof(tail), " [tare %u ns]", static_cast<unsigned>(tare));
                series_print("e2e-local", &g_e2e, tare, tail);
            }
        }
        if (w.valid())
        {
            (void)w.cancel(KOS_CANCEL_KILL, 0);
            (void)w.join(KOS_TIMEOUT_NONE);
        }
        if (note != KOS_CAP_NONE)
        {
            (void)kos_handle_close(note);
        }
        if (irq != KOS_CAP_NONE)
        {
            (void)kos_handle_close(irq);
        }
        if (KICKOS_KERNEL_CORES > 1)
        {
            (void)kos_thread_affinity(self, 0);
        }
    }

    // endpoint_call's D1 donation is guarded on caller_prio strictly greater than
    // server_prio. Both must stay above main (see CR_PRIO).
    void measure_callreply(uint32_t len, uint8_t caller_prio, uint8_t server_prio,
                           uint32_t generic)
    {
        uintptr_t params = len & CR_LEN_MASK;
        if (caller_prio > server_prio)
        {
            params |= CR_DONATING;
        }
        if (generic != 0)
        {
            params |= CR_GENERIC;
        }
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            kos::print("  call/reply: SKIP (no endpoint)\n");
            return;
        }
        kos_cap_grant scaps[] = {{ep, KOS_CAP_WAIT}};   // E(WAIT)@0
        kos_cap_grant ccaps[] = {{ep, KOS_CAP_SIGNAL}}; // E(SIGNAL)@0
        auto sv = kos::thread::create_caps(callreply_server, nullptr, "cr_srv", server_prio, scaps, 1);
        auto cl = kos::thread::create_caps(callreply_caller, reinterpret_cast<void*>(params),
                                           "cr_cl", caller_prio, ccaps, 1);
        if (not sv.valid() or not cl.valid())
        {
            // A lone peer holds its slot for the whole run unless drained HERE: the server
            // needs the sentinel, and the caller is woken refused only once recv_holders
            // reaches 0, which is this close.
            unsigned char stop = 0;
            if (sv.valid())
            {
                (void)kos_send(ep, &stop, 1, KOS_TIMEOUT_NONE);
                (void)sv.join(KOS_TIMEOUT_NONE);
            }
            kos_handle_close(ep);
            if (cl.valid())
            {
                (void)cl.join(KOS_TIMEOUT_NONE);
            }
            kos::print("  call/reply: SKIP (thread pool too small)\n");
            return;
        }
        (void)cl.join(KOS_TIMEOUT_NONE); // caller finished + printed
        (void)sv.join(KOS_TIMEOUT_NONE); // server ran all REPS and exited
        kos_handle_close(ep);
    }

    // The slope across these sizes is TWICE the per-byte cost of the endpoint copy: a round
    // trip copies the request in and the reply back.
    constexpr uint32_t CR_SPANS[] = {8, 16, 32, 64, 128, 256};

    // One size only: the D1 cost does not scale with the message.
    constexpr uint32_t CR_DONATE_SPAN = 32;

    // A spawn costs orders of magnitude more than a switch, so fewer reps carry it.
    constexpr uint32_t SPAWN_REPS = 200;
}

namespace
{
    void row(char const* s)
    {
        kos::print(s);
    }

    // The peer program's tasks: a creating loop that waits for each to die.
    struct XtPeers
    {
        kos_program_t prog = KOS_CAP_NONE;
        kos_task_t a = KOS_TASK_NONE;
        kos_task_t b = KOS_TASK_NONE;

        bool open()
        {
            return kos_program_find("userbench_peer", &prog) == 0;
        }

        void close()
        {
            if (prog != KOS_CAP_NONE)
            {
                (void)kos_handle_close(prog);
                prog = KOS_CAP_NONE;
            }
        }
    };

    void xt_wait(kos_task_t t)
    {
        int status = 0;
        while (kickos::apps::wait_dead(t, &status, 1000u) == -KOS_ETIMEDOUT)
        {
        }
    }

    // Both players run in a task of their own and meet in a block main reserves, which each
    // reaches through the window of its create.
    void xt_pingpong()
    {
        XtPeers peers;
        if (not peers.open())
        {
            row("  xtask ping-pong: SKIP (no peer program)\n");
            return;
        }
        XtWords* const words = static_cast<XtWords*>(kos_ram_alloc(XT_WORDS_BYTES));
        if (words == nullptr or kos_mem_self_grant(words, XT_WORDS_BYTES, 0) != 0)
        {
            row("  xtask ping-pong: SKIP (no block for the words)\n");
            peers.close();
            return;
        }
        (void)kos_sem_init(&words->leader, 0);
        (void)kos_sem_init(&words->follower, 0);
        kos_window const win = {reinterpret_cast<uintptr_t>(words), XT_WORDS_BYTES,
                                KOS_WINDOW_MEMORY, 0};
        // The follower first, so the leader's first post finds it parked or about to park.
        char const* const follow_argv[] = {"xt_b", "follow", nullptr};
        char const* const lead_argv[] = {"xt_a", "lead", nullptr};
        kos_task_params pb = {};
        pb.argv = follow_argv;
        pb.windows = &win;
        pb.window_count = 1;
        kos_task_params pa = {};
        pa.argv = lead_argv;
        pa.windows = &win;
        pa.window_count = 1;
        int const rb = kickos::apps::create_peer(peers.prog, pb, &peers.b);
        int const ra = kickos::apps::create_peer(peers.prog, pa, &peers.a);
        if (ra == 0 and rb == 0)
        {
            xt_wait(peers.a);
            xt_wait(peers.b);
        }
        else
        {
            row("  xtask ping-pong: SKIP (task pool too small)\n");
            if (rb == 0)
            {
                (void)kos_task_slay(peers.b, KOS_TIMEOUT_NONE);
            }
        }
        peers.close();
    }

    // The server and the caller are the two tasks' entries, over an endpoint main made.
    void xt_callreply()
    {
        XtPeers peers;
        if (not peers.open())
        {
            row("  xtask call/reply: SKIP (no peer program)\n");
            return;
        }
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            row("  xtask call/reply: SKIP (no endpoint)\n");
            peers.close();
            return;
        }
        kos_cap_grant const scaps[] = {{ep, KOS_CAP_WAIT}};   // E(WAIT)@0
        kos_cap_grant const ccaps[] = {{ep, KOS_CAP_SIGNAL}}; // E(SIGNAL)@0
        char len_text[16];
        ksnprintf(len_text, sizeof(len_text), "%u",
                  static_cast<unsigned>(XT_SPAN | CR_XTASK));
        char const* const server_argv[] = {"cr_srv", "server", nullptr};
        char const* const caller_argv[] = {"cr_cl", "caller", len_text, nullptr};
        kos_task_params ps = {};
        ps.argv = server_argv;
        ps.caps = scaps;
        ps.cap_count = 1;
        kos_task_params pc = {};
        pc.argv = caller_argv;
        pc.caps = ccaps;
        pc.cap_count = 1;
        int const rs = kickos::apps::create_peer(peers.prog, ps, &peers.a);
        int const rc = kickos::apps::create_peer(peers.prog, pc, &peers.b);
        if (rs == 0 and rc == 0)
        {
            xt_wait(peers.b); // caller finished + printed
            xt_wait(peers.a); // server ran all REPS and exited
        }
        else
        {
            row("  xtask call/reply: SKIP (task pool too small)\n");
            unsigned char stop = 0;
            if (rs == 0)
            {
                (void)kos_send(ep, &stop, 1, KOS_TIMEOUT_NONE);
                xt_wait(peers.a);
            }
        }
        (void)kos_handle_close(ep);
        peers.close();
    }

    void spawn_child(void*)
    {
    }

    void spawn_line(char const* what, uint32_t reps, uint64_t d_ns)
    {
        char s[160];
        if (reps != SPAWN_REPS)
        {
            ksnprintf(s, sizeof(s), "  spawn/exit: %s SKIP (refused after %u reps)\n", what,
                      static_cast<unsigned>(reps));
        }
        else
        {
            ksnprintf(s, sizeof(s), "  spawn/exit: %s %u ns  (%u reps / %u ms)\n", what,
                      static_cast<unsigned>(d_ns / SPAWN_REPS), static_cast<unsigned>(reps),
                      static_cast<unsigned>(d_ns / 1000000ull));
        }
        row(s);
    }

    // A spawn joined before the next, the child outranking main: the round trip is create,
    // first entry, exit and join. The task row adds the task's creation and its release, which
    // on a translating board is a whole address space.
    void spawn_exit()
    {
        uint64_t t0 = kos::clock_now();
        uint32_t n = 0;
        for (; n < SPAWN_REPS; n++)
        {
            auto h = kos::thread::create_caps(spawn_child, nullptr, "sp_t", CR_PRIO, nullptr, 0);
            if (not h.valid() or h.join(KOS_TIMEOUT_NONE) != 0)
            {
                break;
            }
        }
        spawn_line("thread", n, kos::clock_now() - t0);

        XtPeers peers;
        if (not peers.open())
        {
            row("  spawn/exit: task SKIP (no peer program)\n");
            return;
        }
        char const* const exit_argv[] = {"sp_k", "exit", nullptr};
        kos_task_params params = {};
        params.argv = exit_argv;
        t0 = kos::clock_now();
        n = 0;
        for (; n < SPAWN_REPS; n++)
        {
            kos_task_t t = KOS_TASK_NONE;
            if (kickos::apps::create_peer(peers.prog, params, &t) != 0)
            {
                break;
            }
            int status = 0;
            int state = kos_task_state(t, &status);
            while (state >= 0 and (state & KOS_TASK_DEAD) == 0)
            {
                kos_yield();
                state = kos_task_state(t, &status);
            }
            if (state < 0)
            {
                break;
            }
        }
        spawn_line("task", n, kos::clock_now() - t0);
        peers.close();
    }
}

#if BENCH_FLASH_TEXT
namespace
{
    constexpr uint32_t FLASH_ROUNDS = 2000;
    constexpr uint32_t FLASH_CALLS = FLASH_ROUNDS * 8u;
    void (*const g_flash_alias[8])(void) = {
        bench_flash_alias0, bench_flash_alias1, bench_flash_alias2, bench_flash_alias3,
        bench_flash_alias4, bench_flash_alias5, bench_flash_alias6, bench_flash_alias7,
    };
    uint32_t volatile g_flash_mask;

    // The same loop twice: once calling one function, whose line stays cached, once calling the
    // eight in turn.
    uint64_t flash_calls_ns(uint32_t mask)
    {
        g_flash_mask = mask;
        uint64_t const t0 = kos::clock_now();
        for (uint32_t i = 0; i < FLASH_CALLS; i++)
        {
            g_flash_alias[i & g_flash_mask]();
        }
        return kos::clock_now() - t0;
    }

    // Task text runs from flash through the cache: the cost of a call whose line misses.
    void flash_text()
    {
        uint64_t const hit = flash_calls_ns(0u);
        uint64_t const miss = flash_calls_ns(7u);
        char s[160];
        ksnprintf(s, sizeof(s), "  flash text: hit %u ns, miss %u ns per call  (%u calls each)\n",
                  static_cast<unsigned>(hit / FLASH_CALLS), static_cast<unsigned>(miss / FLASH_CALLS),
                  static_cast<unsigned>(FLASH_CALLS));
        row(s);
    }
}
#endif

#if KICKOS_KERNEL_CORES > 1
namespace
{
    // --- the cross-core wake --------------------------------------------------------------
    // main on core 0 and a partner on another core hand a semaphore each way; each hand-over
    // wakes a thread parked on the other core.
    constexpr uint32_t XC_WARMUP = 16;
    constexpr uint8_t XC_PRIO = 3;
    kos_sem_t g_xc_there = KOS_SEM_INIT(0);
    kos_sem_t g_xc_back = KOS_SEM_INIT(0);

    void xc_partner(void*)
    {
        for (uint32_t r = 0; r < XC_WARMUP + SAMPLES; r++)
        {
            if (kos_sem_wait(&g_xc_there, KOS_TIMEOUT_NONE) != 0)
            {
                return;
            }
            kos_sem_post(&g_xc_back);
        }
    }

    bool xc_round()
    {
        kos_sem_post(&g_xc_there);
        return kos_sem_wait(&g_xc_back, KOS_TIMEOUT_NONE) == 0;
    }

    // Half the round trip: per round on a fine clock, over the chained rounds elsewhere.
    void xcore_wake()
    {
        kos_thread_t const self = kos_thread_self();
        (void)kos_thread_affinity(self, 1u << 0);
        for (uint32_t c = 1; c < KICKOS_KERNEL_CORES and c < 32; c++)
        {
            char rowname[32];
            ksnprintf(rowname, sizeof(rowname), "xcore-wake 0-%u", static_cast<unsigned>(c));
            (void)kos_sem_init(&g_xc_there, 0);
            (void)kos_sem_init(&g_xc_back, 0);
            auto p = kos::thread::create_caps(xc_partner, nullptr, "xc_peer", XC_PRIO, nullptr, 0,
                                              KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0, 1u << c);
            if (not p.valid())
            {
                char s[96];
                ksnprintf(s, sizeof(s), "  %s: SKIP (thread pool too small)\n", rowname);
                kos::print(s);
                continue;
            }
            bool ok = true;
            for (uint32_t r = 0; r < XC_WARMUP and ok; r++)
            {
                ok = xc_round();
            }
            Series rt = series(g_samples, samples_kept());
            uint64_t const c0 = kos::clock_now();
            for (uint32_t r = 0; r < SAMPLES and ok; r++)
            {
                if (FINE_CLOCK)
                {
                    uint64_t const t0 = stamp();
                    ok = xc_round();
                    rt.add(span_ns(t0, stamp()) / 2u);
                }
                else
                {
                    ok = xc_round();
                }
            }
            uint64_t const d_ns = kos::clock_now() - c0;
            if (not ok)
            {
                char s[96];
                ksnprintf(s, sizeof(s), "  %s: FAIL (a hand-over was refused)\n", rowname);
                kos::print(s);
                (void)p.cancel(KOS_CANCEL_KILL, 0);
            }
            else if (FINE_CLOCK)
            {
                series_print(rowname, &rt, 0, " [round trip / 2]");
            }
            else
            {
                char s[128];
                ksnprintf(s, sizeof(s), "  %s: %u ns avg  (n=%u) [round trip / 2]\n", rowname,
                          static_cast<unsigned>(d_ns / (2ull * SAMPLES)),
                          static_cast<unsigned>(SAMPLES));
                kos::print(s);
            }
            (void)p.join(KOS_TIMEOUT_NONE);
        }
        (void)kos_thread_affinity(self, 0);
    }

    // --- the scheduler's workloads (docs/design-m9.4-rings.md, section G) ----------------
    // Every thread a workload spawns is joined before the next starts, and all of them outrank
    // main.
    constexpr uint32_t W_ROUNDS = 5000;
    constexpr uint8_t W_PRIO = 3;
    constexpr uint8_t W_HOG_PRIO = 6;
    constexpr uint8_t W_MOVED_PRIO = 4;
    constexpr uint32_t W_RR_QUANTUM_NS = 1000000;
    constexpr uint32_t W_PUSH_ROUNDS = 200;
    // How long the push probe waits on one handoff before it names the round and stops.
    constexpr uint64_t W_PUSH_GIVEUP_NS = 2000000000ull;
    // Spins between clock reads, which are syscalls on some boards: a round that completes
    // inside this many never reads the clock, so a healthy probe adds nothing to what it times.
    constexpr uint32_t W_SPIN_POLL = 1u << 16;
    constexpr uint32_t W_RESEAT_ROUNDS = 500;

    // The ping-pong pairs' words, one couple per pair a workload runs at once; a player's
    // argument is its pair's index.
    constexpr uint32_t W_PAIRS_MAX = 4;
    kos_sem_t g_w_a[W_PAIRS_MAX] = {};
    kos_sem_t g_w_b[W_PAIRS_MAX] = {};

    void w_player_a(void* arg)
    {
        uintptr_t const i = reinterpret_cast<uintptr_t>(arg);
        for (uint32_t r = 0; r < W_ROUNDS; r++)
        {
            kos_sem_wait(&g_w_a[i], KOS_TIMEOUT_NONE);
            kos_sem_post(&g_w_b[i]);
        }
    }

    void w_player_b(void* arg)
    {
        uintptr_t const i = reinterpret_cast<uintptr_t>(arg);
        for (uint32_t r = 0; r < W_ROUNDS; r++)
        {
            kos_sem_post(&g_w_a[i]);
            kos_sem_wait(&g_w_b[i], KOS_TIMEOUT_NONE);
        }
    }

    struct Pair
    {
        kos::thread::Handle a;
        kos::thread::Handle b;
    };

    // Pair `i`'s two players on `mask`; 0 is the default set. Both are joined by the caller.
    bool pair_spawn(uint32_t i, uint32_t mask, Pair* out)
    {
        (void)kos_sem_init(&g_w_a[i], 0);
        (void)kos_sem_init(&g_w_b[i], 0);
        void* const arg = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
        out->a = kos::thread::create_caps(w_player_a, arg, "w_a", W_PRIO, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0, mask);
        out->b = kos::thread::create_caps(w_player_b, arg, "w_b", W_PRIO, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0, mask);
        return out->a.valid() and out->b.valid();
    }

    // A player whose partner was refused waits for good, so a half pair is cancelled first.
    void pair_join(Pair const& p)
    {
        if (p.a.valid() != p.b.valid())
        {
            (void)p.a.cancel(KOS_CANCEL_KILL, 0);
            (void)p.b.cancel(KOS_CANCEL_KILL, 0);
        }
        if (p.a.valid())
        {
            (void)p.a.join(KOS_TIMEOUT_NONE);
        }
        if (p.b.valid())
        {
            (void)p.b.join(KOS_TIMEOUT_NONE);
        }
    }

    void w_line(char const* name, uint32_t switches, uint64_t d_ns)
    {
        char s[128];
        ksnprintf(s, sizeof(s), "  %s: %u switches in %u us\n", name,
                  static_cast<unsigned>(switches), static_cast<unsigned>(d_ns / 1000u));
        kos::print(s);
    }

    void w_rounds_line(char const* name, uint32_t rounds, uint64_t d_ns)
    {
        char s[128];
        ksnprintf(s, sizeof(s), "  %s: %u rounds in %u us\n", name, static_cast<unsigned>(rounds),
                  static_cast<unsigned>(d_ns / 1000u));
        kos::print(s);
    }

    // W2: one pinned pair per core, all at once.
    void w2_pairs()
    {
        uint32_t const n = KICKOS_KERNEL_CORES;
        Pair pairs[W_PAIRS_MAX] = {};
        uint32_t spawned = 0;
        uint64_t const t0 = kos::clock_now();
        for (uint32_t c = 0; c < n and c < W_PAIRS_MAX; c++)
        {
            if (not pair_spawn(c, 1u << c, &pairs[c]))
            {
                break;
            }
            spawned++;
        }
        for (uint32_t c = 0; c < W_PAIRS_MAX; c++)
        {
            pair_join(pairs[c]);
        }
        uint64_t const d = kos::clock_now() - t0;
        w_line("w2", spawned * W_ROUNDS * 2u, d);
    }

    Atomic<uint32_t, Order::RELAXED> g_w_stop{0};

    void w_spinner(void*)
    {
        while (g_w_stop == 0)
        {
        }
    }

    // W3: an unpinned equal-priority pair, with round-robin spinners on wide masks beside it.
    // One default core stays free of spinners: main, below them, must run there to stop them.
    void w3_wide()
    {
        uint32_t n = 0;
        for (uint32_t c = 0; c < KICKOS_KERNEL_CORES and c < 32; c++)
        {
            if ((KOS_ISOLATED_CORES & (1u << c)) == 0)
            {
                n++;
            }
        }
        Pair p = {};
        kos::thread::Handle spin[3] = {};
        g_w_stop = 0;
        uint64_t const t0 = kos::clock_now();
        (void)pair_spawn(0, 0, &p);
        for (uint32_t i = 0; i + 1 < n and i < 3; i++)
        {
            spin[i] = kos::thread::create_caps(w_spinner, nullptr, "w_rr", W_PRIO, nullptr, 0,
                                               KOS_POLICY_RR, W_RR_QUANTUM_NS);
        }
        pair_join(p);
        uint64_t const d = kos::clock_now() - t0;
        g_w_stop = 1;
        w_line("w3", W_ROUNDS * 2u, d);
        for (kos::thread::Handle const& h : spin)
        {
            if (h.valid())
            {
                (void)h.join(KOS_TIMEOUT_NONE);
            }
        }
    }

    // The push probe: every core but core 1 holds a spinning hog above the moved thread, which is
    // readied on core 0 behind its hog and pushed when core 1's hog parks.
    Atomic<uint32_t, Order::RELAXED> g_h1_on{0};
    Atomic<uint32_t, Order::RELAXED> g_park_req{0};
    Atomic<uint32_t, Order::RELAXED> g_moved{0};
    // every probe thread: waited once before it begins
    kos_sem_t g_p_start = KOS_SEM_INIT(0);
    // hog 0 and hog 1: hog 1's release
    kos_sem_t g_p_h1 = KOS_SEM_INIT(0);
    // hog 0 and the moved thread: the moved thread's release
    kos_sem_t g_p_m = KOS_SEM_INIT(0);
    uint32_t g_p_rounds = 0;

    // Spins until `cell` reads `want`, false once W_PUSH_GIVEUP_NS have passed.
    bool p_await(Atomic<uint32_t, Order::RELAXED>& cell, uint32_t want)
    {
        uint64_t deadline = 0;
        uint32_t spins = 0;
        while (cell != want)
        {
            spins++;
            if (spins < W_SPIN_POLL)
            {
                continue;
            }
            spins = 0;
            uint64_t const now = kos::clock_now();
            if (deadline == 0)
            {
                deadline = now + W_PUSH_GIVEUP_NS;
            }
            else if (now >= deadline)
            {
                return false;
            }
        }
        return true;
    }

    void p_stall(uint32_t round, char const* what)
    {
        char s[96];
        ksnprintf(s, sizeof(s), "  push-probe: STALL round=%u/%u (%s)\n",
                  static_cast<unsigned>(round), static_cast<unsigned>(W_PUSH_ROUNDS), what);
        kos::print(s);
    }

    void p_hog0(void*)
    {
        kos_sem_wait(&g_p_start, KOS_TIMEOUT_NONE);
        // A skipped probe releases START with the stop already set, and p_await's spin makes no
        // entry where a pending kill could land.
        uint32_t r = 1;
        for (; r <= W_PUSH_ROUNDS and g_w_stop == 0; r++)
        {
            kos_sem_post(&g_p_h1);
            if (not p_await(g_h1_on, r))
            {
                p_stall(r, "hog 1 never ran");
                break;
            }
            kos_sem_post(&g_p_m);
            g_park_req = r;
            if (not p_await(g_moved, r))
            {
                p_stall(r, "the moved thread was never pushed");
                break;
            }
        }
        g_p_rounds = r - 1u;
        g_w_stop = 1;
        kos_sem_post(&g_p_h1);
        kos_sem_post(&g_p_m);
    }

    void p_hog1(void*)
    {
        kos_sem_wait(&g_p_start, KOS_TIMEOUT_NONE);
        uint32_t r = 0;
        while (true)
        {
            kos_sem_wait(&g_p_h1, KOS_TIMEOUT_NONE);
            if (g_w_stop != 0)
            {
                return;
            }
            r++;
            g_h1_on = r;
            // A stall ends the probe without ever publishing this round's park request.
            while (g_park_req != r and g_w_stop == 0)
            {
            }
        }
    }

    void p_moved(void*)
    {
        kos_sem_wait(&g_p_start, KOS_TIMEOUT_NONE);
        uint32_t r = 0;
        while (true)
        {
            kos_sem_wait(&g_p_m, KOS_TIMEOUT_NONE);
            if (g_w_stop != 0)
            {
                return;
            }
            r++;
            g_moved = r;
        }
    }

    void p_spinner(void*)
    {
        kos_sem_wait(&g_p_start, KOS_TIMEOUT_NONE);
        while (g_w_stop == 0)
        {
        }
    }

    void w4_push()
    {
        uint32_t const n = KICKOS_KERNEL_CORES;
        (void)kos_sem_init(&g_p_start, 0);
        (void)kos_sem_init(&g_p_h1, 0);
        (void)kos_sem_init(&g_p_m, 0);
        g_w_stop = 0;
        g_h1_on = 0;
        g_park_req = 0;
        g_moved = 0;
        g_p_rounds = 0;
        kos::thread::Handle hs[6] = {};
        uint32_t k = 0;
        hs[k++] = kos::thread::create_caps(p_hog0, nullptr, "p_hog0", W_HOG_PRIO, nullptr, 0,
                                           KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0, 1u << 0);
        hs[k++] = kos::thread::create_caps(p_hog1, nullptr, "p_hog1", W_HOG_PRIO, nullptr, 0,
                                           KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0, 1u << 1);
        hs[k++] = kos::thread::create(p_moved, nullptr, "p_moved", W_MOVED_PRIO);
        for (uint32_t c = 2; c < n and k < 6; c++)
        {
            hs[k++] = kos::thread::create_caps(p_spinner, nullptr, "p_spin", W_HOG_PRIO, nullptr,
                                               0, KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0,
                                               1u << c);
        }
        bool all = true;
        for (uint32_t i = 0; i < k; i++)
        {
            if (not hs[i].valid())
            {
                all = false;
            }
        }
        if (all)
        {
            uint64_t const t0 = kos::clock_now();
            for (uint32_t i = 0; i < k; i++)
            {
                kos_sem_post(&g_p_start);
            }
            (void)hs[0].join(KOS_TIMEOUT_NONE);
            w_rounds_line("w4-push", g_p_rounds, kos::clock_now() - t0);
        }
        else
        {
            kos::print("  push-probe: SKIP (thread pool too small)\n");
            g_w_stop = 1;
            for (uint32_t i = 0; i < k; i++)
            {
                kos_sem_post(&g_p_start);
            }
            // The two releases are the threads' own now, so a thread still waiting on one is
            // cancelled instead.
            for (uint32_t i = 0; i < k; i++)
            {
                (void)hs[i].cancel(KOS_CANCEL_KILL, 0);
            }
        }
        for (uint32_t i = 0; i < k; i++)
        {
            if (hs[i].valid())
            {
                (void)hs[i].join(KOS_TIMEOUT_NONE);
            }
        }
    }

    // The reseat probe: a thread spinning on core 1, re-masked from this core over and over with
    // core 1 in every mask, so each request is re-seated by that core with the thread running.
    void w4_reseat()
    {
        g_w_stop = 0;
        auto t = kos::thread::create_caps(w_spinner, nullptr, "r_spin", W_MOVED_PRIO, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0, 1u << 1);
        if (not t.valid())
        {
            kos::print("  reseat-probe: SKIP (thread pool too small)\n");
            return;
        }
        uint64_t const t0 = kos::clock_now();
        for (uint32_t r = 0; r < W_RESEAT_ROUNDS; r++)
        {
            uint32_t mask = (1u << 1) | (1u << 0);
            if ((r & 1u) != 0)
            {
                mask = 1u << 1;
            }
            (void)kos_thread_affinity(t.id(), mask);
            kos_yield();
        }
        w_rounds_line("w4-reseat", W_RESEAT_ROUNDS, kos::clock_now() - t0);
        g_w_stop = 1;
        (void)t.join(KOS_TIMEOUT_NONE);
    }
}
#endif

int main(int, char**)
{
    kos::print("microbenchmark: switch, IPC, spawn and wake costs, each timed from user space\n");

    uint64_t const hz = kos_cpu_clock_hz();
    char line[160];
    ksnprintf(line, sizeof(line), "cpu clock: %llu Hz (0 = the backend states none)\n",
              static_cast<unsigned long long>(hz));
    kos::print(line);
    if (not clock_seat(hz))
    {
        kos::print("bench: FAILED (no CPU clock rate converts a cycle stamp)\n");
        return 1;
    }
    char const* clock = "average";
    if (FINE_CLOCK)
    {
        clock = "fine";
    }
    char const* vector = "no";
    if (BENCH_VEC_DIRTY)
    {
        vector = "yes";
    }
    char const* aspace = "no";
    if (KICKOS_HAVE_ASPACE)
    {
        aspace = "yes";
    }
    char const* flash = "no";
    if (BENCH_FLASH_TEXT)
    {
        flash = "yes";
    }
    ksnprintf(line, sizeof(line),
              "bench: posture: cores %u, clock %s, vector %s, aspace %s, flash %s, threads %u\n\n",
              static_cast<unsigned>(KICKOS_KERNEL_CORES), clock, vector, aspace, flash,
              static_cast<unsigned>(USERBENCH_THREADS));
    kos::print(line);

    uint32_t const tare = clock_tare(g_samples, samples_kept(), TARE_SAMPLES);
    bench::walks(g_samples, tare);

    // Each step joins BOTH its peers before the next starts, so two pool slots beside
    // main's carry the whole sweep.
    for (unsigned i = 0; i < sizeof(CR_SPANS) / sizeof(CR_SPANS[0]); i++)
    {
        measure_callreply(CR_SPANS[i], CR_PRIO, CR_PRIO, 0);
    }

    // Above KOS_CALL_REG_BYTES kos_call already issues this same trap, so those rows must
    // differ from the register arm by one flat constant, the failed register-form attempt.
    for (unsigned i = 0; i < sizeof(CR_SPANS) / sizeof(CR_SPANS[0]); i++)
    {
        measure_callreply(CR_SPANS[i], CR_PRIO, CR_PRIO, 1);
    }

    measure_callreply(CR_DONATE_SPAN, CR_PRIO + 1u, CR_PRIO, 0);
    kos::print("\n");

    // Players at prio 1 (KICKOS_PRIO_MIN), below main's prio 2, so the reporter preempts
    // them when player_b posts the gate.
    auto ra = kos::thread::create_caps(player_a, nullptr, "bench_a", 1, nullptr, 0,
                                       KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0, PLAYER_CORES);
    auto rb = kos::thread::create_caps(player_b, nullptr, "bench_b", 1, nullptr, 0,
                                       KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0, PLAYER_CORES);
    if (not ra.valid() or not rb.valid())
    {
        // Do not park here: on a board that returns to its bootloader a parked app costs a
        // physical button press, which the bounded reporter exists to avoid.
        kos::print("bench: FAILED to spawn players (thread pool too small?)\n");
        return 1;
    }

    switch_vec(reporter_loop());
    (void)ra.cancel(KOS_CANCEL_KILL, 0);
    (void)rb.cancel(KOS_CANCEL_KILL, 0);
    (void)ra.join(KOS_TIMEOUT_NONE);
    (void)rb.join(KOS_TIMEOUT_NONE);

    e2e_local(tare);
#if KICKOS_KERNEL_CORES > 1
    xcore_wake();
    w2_pairs();
    w3_wide();
    w4_push();
    w4_reseat();
#endif
    xt_pingpong();
    xt_callreply();
    spawn_exit();
#if BENCH_FLASH_TEXT
    flash_text();
#endif
    kos::print("bench: done\n");
    return 0;
}
