// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Context-switch microbenchmark (KICKOS_BENCH builds only). Two equal-priority
// threads ping-pong via semaphores; the reporter prints throughput (ctx-switches/s via
// kos::clock_now, works on every arch) plus per-switch cost + IRQ-entry latency (cycles,
// only where switch.S brackets the swap with a counter: armv7m DWT, rxv3 CMTW1,
// rv32imac rdcycle/MTIME, xtensa CCOUNT; absent on M0/sim/frozen-QEMU).
//
// The call/reply sweep times the endpoint copy under the kernel's own IrqLock. The phase
// table printed after it breaks that round trip's FIXED cost down by kernel phase
// (kernel/bench/bench.cc).
//
// Every cycle metric goes through kos_bench, one syscall: the helpers behind it read kernel
// .data and core peripherals, and root is unprivileged on every board except the LX6, which
// has no privilege ring.
//
// The app's own lines go through kickos::emit, which reaches a published console driver
// over IPC and falls back to the kernel console when index 0 is empty. kos_print alone
// would be dropped outright once a service list publishes the UART (sys/emit.h).
//
// What emit CANNOT rescue is the switch line and the phase table: the KERNEL prints those
// from inside kos_bench, straight at the kernel console. So an instrumented run still
// wants kickos_services_none, which the `bench` config variant pins.
//
// The reporter is woken by the workload, not by a timer. Telemetry OFF for clean numbers.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/emit.h>
#include <kickos/libc/fmt.h>

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // Any line the bench does not otherwise use (NOT a console TX drain line).
    constexpr int BENCH_IRQ_LINE = 20;
    constexpr int IRQ_SAMPLES = 100;
    // Rounds the players run before waking the reporter: a throughput window of a
    // fraction of a second on fast silicon, a few seconds on a slow M0.
    constexpr uint32_t ROUNDS_PER_REPORT = 20000;
    // Bounded so main RETURNS: root's exit reaches kickos_terminate, and a board built
    // with KICKOS_SHUTDOWN_TO_BOOTLOADER re-enters its bootloader instead of needing a
    // physical button press for the next capture.
    constexpr unsigned THROUGHPUT_REPORTS = 3;

    kos::Semaphore* g_a = nullptr;    // MAIN's caps
    kos::Semaphore* g_b = nullptr;
    kos::Semaphore* g_gate = nullptr;
    Atomic<uint32_t, Order::RELAXED> g_rounds{0};

    // Child cap indices (a fresh child table makes handle == index). MAIN delegates the
    // sems per spawn in this order.
    constexpr int CH_A = 1;    // ping-pong sem A (delegated first to both players)
    constexpr int CH_B = 2;    // ping-pong sem B (delegated second)
    constexpr int CH_GATE = 3; // reporter-wake gate (delegated third to player_b only)
    constexpr uint8_t CH_FULL = KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

    // A refused op returns -KOS_E*; the cycle ops answer 0 for "did not fire", so a
    // refusal must collapse to 0 for a caller's `!= 0` fired-check to hold.
    uint32_t bench_u32(uint32_t op, uint32_t a0, uint32_t a1)
    {
        int32_t const rc = kos_bench(op, a0, a1);
        if (rc < 0)
        {
            return 0;
        }
        return static_cast<uint32_t>(rc);
    }

    void player_a(void*) // caps: A@1, B@2
    {
        while (true)
        {
            kos_sem_wait(CH_A);
            kos_sem_post(CH_B);
        }
    }
    void player_b(void*) // caps: A@1, B@2, gate@3
    {
        while (true)
        {
            kos_sem_wait(CH_B);
            kos_sem_post(CH_A);
            uint32_t const round = g_rounds + 1;
            g_rounds = round;
            if ((round % ROUNDS_PER_REPORT) == 0)
            {
                kos_sem_post(CH_GATE);
            }
        }
    }

    uint32_t to_ns(uint32_t cyc, uint32_t hz)
    {
        if (hz == 0)
        {
            return 0;
        }
        return static_cast<uint32_t>((static_cast<uint64_t>(cyc) * 1000000000ull) / hz);
    }

    // The reporter is the ROOT thread, so the bench needs only 2 pool slots and fits
    // boards with KICKOS_MAX_THREADS as low as 2 (nrf51, stm32f103/f302).
    void reporter_loop(uint32_t hz)
    {
        (void)kos_bench(KOS_BENCH_OP_IRQ_SETUP, BENCH_IRQ_LINE, 0); // no-op if refused
        uint32_t prev_rounds = g_rounds;
        uint64_t prev_ns = kos::clock_now();
        g_a->post();

        for (unsigned rep = 0; rep < THROUGHPUT_REPORTS; rep++)
        {
            (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0);
            g_gate->wait();

            // The window is the players' burst alone: prev_* is sampled at the END of the
            // previous iteration, excluding the reporter's own report time.
            uint64_t now_ns = kos::clock_now();
            uint32_t rounds = g_rounds;
            uint64_t switches = static_cast<uint64_t>(rounds - prev_rounds) * 2ull; // 2 switches/round
            uint64_t d_ns = now_ns - prev_ns;

            uint32_t sw_per_s = 0;
            uint32_t ns_per_sw = 0;
            if (d_ns != 0 and switches != 0)
            {
                sw_per_s = static_cast<uint32_t>(switches * 1000000000ull / d_ns);
                ns_per_sw = static_cast<uint32_t>(d_ns / switches);
            }

            // Every %u argument is cast to `unsigned`: uint32_t is `unsigned long` on the
            // newlib targets and plain `unsigned` on the host and Xtensa, and `unsigned` is
            // 32-bit everywhere KickOS runs.
            char s[160];
            ksnprintf(s, sizeof(s),
                      "  throughput: %u ctx-sw/s  (%u ns/sw avg over %u switches / %u ms)\n",
                      static_cast<unsigned>(sw_per_s), static_cast<unsigned>(ns_per_sw),
                      static_cast<unsigned>(switches),
                      static_cast<unsigned>(d_ns / 1000000ull));
            kickos::emit(s);

            // Cycles only where switch.S bracketed them. The kernel writes the switch
            // line itself and hands back the sample count.
            uint32_t const scnt = bench_u32(KOS_BENCH_OP_SWITCH_PRINT, 0, 0);
            if (scnt == 0)
            {
                // no cycle counter on this arch; throughput is the metric
                prev_rounds = g_rounds;
                prev_ns = kos::clock_now();
                continue;
            }

            uint32_t imin = 0xFFFFFFFFu, imax = 0, icnt = 0;
            uint64_t isum = 0;
            for (int i = 0; i < IRQ_SAMPLES; i++)
            {
                uint32_t c = bench_u32(KOS_BENCH_OP_IRQ_ONCE, BENCH_IRQ_LINE, 0);
                if (c != 0)
                {
                    if (c < imin) { imin = c; }
                    if (c > imax) { imax = c; }
                    isum += c;
                    icnt++;
                }
            }
            uint32_t iavg = 0;
            if (icnt != 0)
            {
                iavg = static_cast<uint32_t>(isum / icnt);
            }
            else
            {
                imin = 0;
            }

            ksnprintf(s, sizeof(s), "  irq:    %u/%u/%u cyc  %u/%u/%u ns  (min/avg/max, n=%u)\n",
                      static_cast<unsigned>(imin), static_cast<unsigned>(iavg),
                      static_cast<unsigned>(imax), static_cast<unsigned>(to_ns(imin, hz)),
                      static_cast<unsigned>(to_ns(iavg, hz)),
                      static_cast<unsigned>(to_ns(imax, hz)), static_cast<unsigned>(icnt));
            kickos::emit(s);

            // Worst-case inject->entry: the line is raised at the START of a masked span
            // of the given size (0 = fixed cost; 256 = endpoint-copy max). A frozen counter
            // reports ~1 cyc on mps2 DWT, same as the best-case line above.
            static const uint32_t wspans[] = {0, 64, 256, 1024};
            for (unsigned si = 0; si < sizeof(wspans) / sizeof(wspans[0]); si++)
            {
                uint32_t wmin = 0xFFFFFFFFu, wmax = 0, wcnt = 0;
                uint64_t wsum = 0;
                for (int k = 0; k < IRQ_SAMPLES; k++)
                {
                    uint32_t c =
                        bench_u32(KOS_BENCH_OP_IRQ_MASKED_ONCE, BENCH_IRQ_LINE, wspans[si]);
                    if (c != 0)
                    {
                        if (c < wmin) { wmin = c; }
                        if (c > wmax) { wmax = c; }
                        wsum += c;
                        wcnt++;
                    }
                }
                if (wcnt == 0)
                {
                    continue;
                }
                uint32_t wavg = static_cast<uint32_t>(wsum / wcnt);
                ksnprintf(s, sizeof(s),
                          "  wcase-irq[%uB]: %u/%u/%u cyc  %u/%u/%u ns  (inject->entry, n=%u)\n",
                          static_cast<unsigned>(wspans[si]), static_cast<unsigned>(wmin),
                          static_cast<unsigned>(wavg), static_cast<unsigned>(wmax),
                          static_cast<unsigned>(to_ns(wmin, hz)),
                          static_cast<unsigned>(to_ns(wavg, hz)),
                          static_cast<unsigned>(to_ns(wmax, hz)), static_cast<unsigned>(wcnt));
                kickos::emit(s);
            }

            // AFTER the report, so the next window excludes this report's own
            // IRQ-sampling and print time.
            prev_rounds = g_rounds;
            prev_ns = kos::clock_now();
        }
    }

    // Call/reply round-trip: the same 2-switches-per-round handoff as the sem ping-pong
    // above. Both peers are SPAWNED, so the figure is worker-to-worker and not root's.
    constexpr uint32_t CALLREPLY_REPS = 20000;

    // A peer MUST outrank root (prio KICKOS_PRIO_MIN + 1 == 2): it posts `done` as its last
    // act but reaches EXITED only afterwards, and root preempting it on that post leaves the
    // peer READY, holding a slot ThreadPool::alloc cannot reclaim for the next sweep step.
    // On a 3-slot pool that spawn is -KOS_ENOMEM.
    constexpr uint8_t CR_PRIO = 4;

    // Written by measure_callreply BEFORE it spawns either peer, so neither peer races it.
    Atomic<uint32_t, Order::RELAXED> g_cr_len{16};

    // Same write discipline as g_cr_len.
    Atomic<uint32_t, Order::RELAXED> g_cr_donating{0};

    // Non-zero puts the caller on kos_call_generic, which issues KOS_SYS_CALL without
    // attempting the register form. Same write discipline as g_cr_len.
    Atomic<uint32_t, Order::RELAXED> g_cr_generic{0};

    void callreply_server(void*) // caps: E(WAIT)@1, done@2
    {
        unsigned char buf[KOS_EP_MSG_MAX];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        for (uint32_t i = 0; i < CALLREPLY_REPS; i++)
        {
            long n = kos_recv(1, buf, sizeof(buf), &info);
            if (n < 0 or info.reply_cap == KOS_CAP_NONE)
            {
                // A reply-less message is measure_callreply's stop sentinel: a parked
                // receiver pins its own WAIT cap, so nothing but a message ends that park.
                break;
            }
            kos_reply(info.reply_cap, buf, static_cast<size_t>(n));
        }
        kos_sem_post(2);
    }
    void callreply_caller(void*) // caps: E(SIGNAL)@1, done@2
    {
        unsigned char buf[KOS_EP_MSG_MAX];
        size_t const len = static_cast<size_t>(g_cr_len);
        for (unsigned i = 0; i < sizeof(buf); i++)
        {
            buf[i] = static_cast<unsigned char>(i);
        }
        // Selected OUTSIDE the timed loop, so neither arm carries the other's branch.
        bool const generic = (g_cr_generic != 0);
        uint64_t t0 = kos::clock_now();
        uint32_t reps = 0;
        if (generic)
        {
            while (reps < CALLREPLY_REPS)
            {
                if (kos_call_generic(1, buf, len, len) < 0)
                {
                    break;
                }
                reps++;
            }
        }
        else
        {
            while (reps < CALLREPLY_REPS)
            {
                // In place: the reply lands back in the request buffer.
                if (kos_call(1, buf, len, len) < 0)
                {
                    break;
                }
                reps++;
            }
        }
        uint64_t d_ns = kos::clock_now() - t0;

        if (reps == CALLREPLY_REPS)
        {
            uint32_t rt_per_s = 0;
            uint32_t ns_per_rt = 0;
            if (d_ns != 0)
            {
                rt_per_s = static_cast<uint32_t>(
                    static_cast<uint64_t>(CALLREPLY_REPS) * 1000000000ull / d_ns);
                ns_per_rt = static_cast<uint32_t>(d_ns / CALLREPLY_REPS);
            }
            char const* shape = "";
            if (g_cr_donating != 0)
            {
                shape = " [caller outranks server, D1 donates]";
            }
            char const* path = "";
            if (generic)
            {
                path = " [generic path]";
            }
            char s[256];
            ksnprintf(
                s, sizeof(s),
                "  call/reply: %u B  %u ns/round-trip  (%u round-trips/s over %u calls / %u ms)%s%s\n",
                static_cast<unsigned>(len), static_cast<unsigned>(ns_per_rt),
                static_cast<unsigned>(rt_per_s), static_cast<unsigned>(CALLREPLY_REPS),
                static_cast<unsigned>(d_ns / 1000000ull), path, shape);
            kickos::emit(s);
        }
        kos_sem_post(2);
    }
    // endpoint_call's D1 donation is guarded on caller_prio strictly greater than
    // server_prio, so an equal-priority step leaves that phase row with zero samples.
    // Both must stay above root (see CR_PRIO).
    void measure_callreply(uint32_t len, uint8_t caller_prio, uint8_t server_prio,
                           uint32_t generic)
    {
        g_cr_len = len;
        uint32_t donating = 0;
        if (caller_prio > server_prio)
        {
            donating = 1;
        }
        g_cr_donating = donating;
        g_cr_generic = generic;
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            kickos::emit("  call/reply: SKIP (no endpoint)\n");
            return;
        }
        kos::Semaphore done(0);
        kos_cap_grant scaps[] = {{ep, KOS_CAP_WAIT}, {done.id(), CH_FULL}};   // E(WAIT)@1, done@2
        kos_cap_grant ccaps[] = {{ep, KOS_CAP_SIGNAL}, {done.id(), CH_FULL}}; // E(SIGNAL)@1, done@2
        auto sv = kos::thread::spawn_caps(callreply_server, nullptr, "cr_srv", server_prio, scaps, 2);
        auto cl = kos::thread::spawn_caps(callreply_caller, nullptr, "cr_cl", caller_prio, ccaps, 2);
        if (not sv.valid() or not cl.valid())
        {
            // A lone peer holds its slot for the whole run unless drained HERE: the server
            // needs the sentinel, and the caller wakes -KOS_EPIPE only once recv_holders
            // reaches 0, which is this close.
            unsigned char stop = 0;
            if (sv.valid())
            {
                (void)kos_send(ep, &stop, 1);
                done.wait();
            }
            kos_handle_close(ep);
            if (cl.valid())
            {
                done.wait();
            }
            kickos::emit("  call/reply: SKIP (thread pool too small)\n");
            return;
        }
        done.wait(); // caller finished + printed
        done.wait(); // server ran all REPS and exited
        kos_handle_close(ep);
    }

    // The slope across these sizes is TWICE the per-byte cost of the endpoint copy: a round
    // trip copies the request in and the reply back, both under one IrqLock.
    constexpr uint32_t CR_SPANS[] = {8, 16, 32, 64, 128, 256};

    // One size only: the D1 cost does not scale with the message.
    constexpr uint32_t CR_DONATE_SPAN = 32;
}

int main(int, char**)
{
    kickos::emit("microbenchmark: context-switch throughput (all arches) + per-switch cost\n");
    kickos::emit("+ IRQ-entry latency where a cycle counter exists. Reporter woken by the\n");
    kickos::emit("workload, not a timer. Telemetry OFF for clean numbers.\n");

    uint32_t const hz = bench_u32(KOS_BENCH_OP_CORE_HZ, 0, 0);
    char hzline[80];
    ksnprintf(hzline, sizeof(hzline), "core clock: %u Hz (0 = the chip backend does not say)\n\n",
              static_cast<unsigned>(hz));
    kickos::emit(hzline);

    (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0); // the phase table below covers the sweep only

    // Each step joins BOTH its peers before the next starts, so two pool slots beside
    // root's carry the whole sweep.
    for (unsigned i = 0; i < sizeof(CR_SPANS) / sizeof(CR_SPANS[0]); i++)
    {
        measure_callreply(CR_SPANS[i], CR_PRIO, CR_PRIO, 0);
    }

    // The generic arm of the spans the register form can carry, so one run holds both sides
    // of the fastpath comparison. Above KOS_CALL_REG_BYTES kos_call already issues this same
    // trap, and on a backend with no fastpath the two arms are the same code either way.
    for (unsigned i = 0; i < sizeof(CR_SPANS) / sizeof(CR_SPANS[0]); i++)
    {
        if (CR_SPANS[i] <= static_cast<uint32_t>(KOS_CALL_REG_BYTES))
        {
            measure_callreply(CR_SPANS[i], CR_PRIO, CR_PRIO, 1);
        }
    }

    // Read the phase table's donate row against the equal-priority rows above.
    measure_callreply(CR_DONATE_SPAN, CR_PRIO + 1u, CR_PRIO, 0);
    (void)kos_bench(KOS_BENCH_OP_PHASE_PRINT, 0, 0); // the kernel writes the table
    kickos::emit("\n");

    kos::Semaphore a(0), b(0), gate(0);
    g_a = &a;
    g_b = &b;
    g_gate = &gate;

    // Players at prio 1 (KICKOS_PRIO_MIN), below root's prio 2, so the reporter preempts
    // them when player_b posts the gate.
    kos_cap_grant acaps[] = {{a.id(), CH_FULL}, {b.id(), CH_FULL}};                    // A@1, B@2
    kos_cap_grant bcaps[] = {{a.id(), CH_FULL}, {b.id(), CH_FULL}, {gate.id(), CH_FULL}}; // +gate@3
    auto ra = kos::thread::spawn_caps(player_a, nullptr, "bench_a", 1, acaps, 2);
    auto rb = kos::thread::spawn_caps(player_b, nullptr, "bench_b", 1, bcaps, 3);
    if (not ra.valid() or not rb.valid())
    {
        // Do not park here: on a bootloader-handover board a parked app costs a physical
        // button press, which the bounded reporter exists to avoid.
        kickos::emit("bench: FAILED to spawn players (thread pool too small?)\n");
        return 1;
    }

    reporter_loop(hz);
    kickos::emit("bench: done\n");
    return 0;
}
