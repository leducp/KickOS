// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Concurrency stress test (bounded, self-verifying). Hammers the three subsystems
// that carry the subtle races -- the scheduler, the counting semaphores, and the
// tickless timer -- with many threads interleaving at mixed priorities, then
// asserts exact conservation counts. A lost wakeup, a dropped timer deadline, or a
// wait-queue corruption shows up as either a hang (the join never completes -> the
// harness times out) or a final count mismatch (STRESS FAIL). Terminates cleanly
// (every worker exits, main verifies and returns its failure count), so it doubles
// as a CTest gate the way selftest does. Prints "# stress complete" only after a
// full clean run.
//
// Pool use scales to the board: the soak first PROBES the concurrent thread budget
// (spawn parked threads until one is refused), then sizes the ping-pong pairs and
// sleepers to fit it -- so it runs a real soak on ANY pool (sim 16, XMC 8, ...)
// instead of SKIPping the small boards. idle/root are separate static TCBs, not
// pool-allocated. The sim keeps its historical 3 pairs + 6 sleepers footprint (the
// budget only ever shrinks it), so the CI gate is unchanged. Every create/spawn is
// still checked: a board too small even for one pair SKIPs rather than hanging a join.
//
// Then a spawn/exit churn phase: live*CHURN_GENERATIONS spawn/exit cycles, at most
// `live` (the just-freed conservation set) concurrent, each batch joined before the
// next spawns. With a broken reclaim the pool exhausts and a spawn returns -1 ->
// STRESS FAIL (not a hang); every reused slot re-runs the full TCB/context reset.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

namespace
{
    constexpr int MAX_PAIRS = 3;      // ping-pong pairs on a large pool (array sizing)
    constexpr int MAX_SLEEPERS = 6;   // concurrent sleepers on a large pool
    constexpr int PROBE_MAX = 64;     // cap on the budget probe (any real pool <= this)
    constexpr int ROUNDS = 3000;      // handoffs per ping-pong thread
    constexpr int NAPS = 100;         // sleeps per sleeper
    constexpr int CHURN_GENERATIONS = 170; // ~2000 spawn/exit cycles total
    // pairs/sleepers/churn-batch are sized at runtime from the probed budget (main).
    constexpr uint64_t NAP_MIN_NS = 80000ull;    // 80 us
    constexpr uint64_t NAP_SPAN_NS = 400000ull;  // + up to 400 us

    int g_done = -1; // completion counter (each worker posts once at exit) -- MAIN's cap
    int g_mtx = -1;  // binary sem guarding the shared counters -- MAIN's cap
    int g_gate = -1; // budget-probe gate (probers park here until released) -- MAIN's cap

    int g_pair_a[MAX_PAIRS];   // "ping waits A, posts B" -- MAIN's caps
    int g_pair_b[MAX_PAIRS];

    // B1 well-known child cap indices (fresh child table => handle == index; delegated
    // cap i -> index i+1). MAIN owns the sems and delegates them per spawn in a fixed
    // order so the worker helpers can name them by these constants.
    constexpr int CH_DONE = 1; // completion counter, delegated FIRST to every worker
    constexpr int CH_MTX = 2;  // counter mutex, delegated SECOND (all conservation workers)
    constexpr int CH_A = 3;    // ping-pong pair sem A (delegated THIRD to ping/pong)
    constexpr int CH_B = 4;    // ping-pong pair sem B (delegated FOURTH to ping/pong)
    constexpr int CH_GATE = 2; // budget-probe gate for the prober (done@1, gate@2)
    constexpr uint8_t CH_FULL = KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

    // Shared conservation counters (g_mtx serializes every access).
    long g_naps_done = 0;    // total sleeper wakes; expect sleepers*NAPS
    long g_handoffs = 0;     // total ping-pong handoffs; expect 2*pairs*ROUNDS
    long g_churn_runs = 0;   // total churn-worker runs; expect live*CHURN_GENERATIONS

    // Called only from worker threads: names the counter mutex by its delegated cap.
    void lock() { kos_sem_wait(CH_MTX); }
    void unlock() { kos_sem_post(CH_MTX); }

    // Per-thread LCG (seeded off the thread index): deterministic, no shared state.
    uint32_t lcg(uint32_t* s)
    {
        *s = *s * 1103515245u + 12345u;
        return *s;
    }

    // caps: done@1, mtx@2, pair-A@3 (CH_A), pair-B@4 (CH_B). arg = pair index (LCG seed only).
    void ping(void* arg)
    {
        int i = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        uint32_t seed = 0x9E3779B9u ^ (static_cast<uint32_t>(i) * 2654435761u);
        for (int r = 0; r < ROUNDS; r++)
        {
            kos_sem_wait(CH_A);
            // Occasional nap: interleave the sem path with the tickless one-shot.
            if ((r & 63) == 0)
            {
                kos_sleep_ns(NAP_MIN_NS + (lcg(&seed) % NAP_SPAN_NS));
            }
            kos_sem_post(CH_B);
            lock();
            g_handoffs++;
            unlock();
        }
        kos_sem_post(CH_DONE);
    }

    // caps: done@1, mtx@2, pair-A@3 (CH_A), pair-B@4 (CH_B).
    void pong(void*)
    {
        for (int r = 0; r < ROUNDS; r++)
        {
            kos_sem_wait(CH_B);
            kos_sem_post(CH_A);
            lock();
            g_handoffs++;
            unlock();
        }
        kos_sem_post(CH_DONE);
    }

    // caps: done@1, mtx@2.
    void sleeper(void* arg)
    {
        int i = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        uint32_t seed = 0x1234567u + static_cast<uint32_t>(i) * 40503u;
        for (int j = 0; j < NAPS; j++)
        {
            kos_sleep_ns(NAP_MIN_NS + (lcg(&seed) % NAP_SPAN_NS));
            lock();
            g_naps_done++;
            unlock();
        }
        kos_sem_post(CH_DONE);
    }

    // Spawn/exit churn worker: bump the counter, signal done, return (the arch
    // trampoline routes the return through exit_current -> EXITED). MUST run above
    // the driver's priority (root = KICKOS_PRIO_MIN+1): that is the guarantee it
    // reaches EXITED before the driver is rescheduled to spawn the next batch, so its
    // slot is reclaimable in time. Below root, reclaim can miss it -> spurious FAIL.
    // caps: done@1, mtx@2.
    void churner(void*)
    {
        lock();
        g_churn_runs++;
        unlock();
        kos_sem_post(CH_DONE);
    }

    // Budget probe: park on the gate until main releases it, then exit. Spawned
    // until one is refused, so the live count == the board's concurrent thread pool.
    // caps: done@1, gate@2 (CH_GATE).
    void prober(void*)
    {
        kos_sem_wait(CH_GATE);
        kos_sem_post(CH_DONE);
    }

    // Concurrent thread budget = how many prober threads can be live at once (idle
    // and root are static TCBs, off the pool). Spawn until refused, then release +
    // join them all, leaving the pool empty again.
    int probe_budget()
    {
        g_gate = kos_sem_create(0);
        if (g_gate < 0)
        {
            return 0;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_gate, CH_FULL}}; // done@1, gate@2
        int n = 0;
        while (n < PROBE_MAX)
        {
            int t = kos::thread::spawn_caps(prober, nullptr, "probe", 4, caps, 2);
            if (t < 0)
            {
                break;
            }
            n++;
        }
        for (int i = 0; i < n; i++)
        {
            kos_sem_post(g_gate); // release every parked prober
        }
        for (int i = 0; i < n; i++)
        {
            kos_sem_wait(g_done); // join (also reclaims their slots)
        }
        kos_sem_destroy(g_gate);
        g_gate = -1;
        return n;
    }
}

// One full conservation round, repeatable: resets the accumulators, allocates THIS
// round's per-pair sems, spawns + kicks + joins the conservation set, runs the churn
// phase, checks exact conservation, then destroys the per-pair sems it created so the
// sem pool returns to exactly what it was on entry. g_done/g_mtx are MAIN's and are
// NOT touched here (they self-balance: every worker posts g_done once and is joined;
// g_mtx is released as often as taken). Returns the failure count for this round.
int run_stress_round(int pairs, int sleepers, int live)
{
    g_naps_done = 0;
    g_handoffs = 0;
    g_churn_runs = 0;

    bool ok = true;
    int made_pairs = 0; // pairs whose BOTH sems were created -- destroy exactly these
    int spawned = 0;

    // Mixed priorities straddling the sleepers' band; the last pair is RR.
    for (int i = 0; ok and i < pairs; i++)
    {
        g_pair_a[i] = kos_sem_create(0);
        g_pair_b[i] = kos_sem_create(0);
        if (g_pair_a[i] < 0 or g_pair_b[i] < 0)
        {
            ok = false;
            break;
        }
        made_pairs++;
        uint8_t prio = static_cast<uint8_t>(8 + i); // 8,9,10
        uint8_t policy = KOS_POLICY_FIFO;
        uint32_t quantum = 0;
        if (i == pairs - 1)
        {
            policy = KOS_POLICY_RR;
            quantum = 300000u; // 300 us
        }
        // done@1, mtx@2, this pair's A@3, B@4.
        kos_cap_grant pcaps[] = {{g_done, CH_FULL}, {g_mtx, CH_FULL},
                                 {g_pair_a[i], CH_FULL}, {g_pair_b[i], CH_FULL}};
        int a = kos::thread::spawn_caps(ping, reinterpret_cast<void*>(uintptr_t(i)), "ping",
                                        prio, pcaps, 4, policy, quantum, /*privileged=*/true);
        int b = kos::thread::spawn_caps(pong, reinterpret_cast<void*>(uintptr_t(i)), "pong",
                                        prio, pcaps, 4, policy, quantum, /*privileged=*/true);
        if (a < 0 or b < 0)
        {
            ok = false;
            break;
        }
        spawned += 2;
    }
    for (int i = 0; ok and i < sleepers; i++)
    {
        uint8_t prio = static_cast<uint8_t>(6 + (i % 6)); // 6..11
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_mtx, CH_FULL}}; // done@1, mtx@2
        int t = kos::thread::spawn_caps(sleeper, reinterpret_cast<void*>(uintptr_t(i)), "sleep",
                                        prio, scaps, 2);
        if (t < 0)
        {
            ok = false;
            break;
        }
        spawned++;
    }

    // A mid-round create/spawn failure means the pool did not return to its start
    // state (a leak): a hard FAIL, not a SKIP -- main already proved the budget fits
    // on entry. Do NOT join a half-spawned set (a lone ping would hang the join);
    // reclaim the sems we created and report the failure so the soak halts on it.
    if (not ok or spawned != live)
    {
        for (int i = 0; i < made_pairs; i++)
        {
            kos_sem_destroy(g_pair_a[i]);
            kos_sem_destroy(g_pair_b[i]);
        }
        return 1;
    }

    // Kick each pair once; the token then circulates ROUNDS times per side.
    for (int i = 0; i < pairs; i++)
    {
        kos_sem_post(g_pair_a[i]);
    }

    // Join every worker. A lost wakeup / dropped deadline hangs here -> the harness
    // timeout catches it as a failure.
    for (int i = 0; i < spawned; i++)
    {
        kos_sem_wait(g_done);
    }

    // Churn phase: the conservation workers above are all EXITED now, so their pool
    // slots must be reclaimed here -- live*CHURN_GENERATIONS >> the pool. A spawn
    // returning -1 means reclamation failed (pool exhausted): a real FAIL, not a
    // SKIP. Each batch is joined before the next spawns, so at most `live` are ever
    // concurrent and the join never waits on a thread that was not created.
    bool churn_ok = true;
    for (int g = 0; churn_ok and g < CHURN_GENERATIONS; g++)
    {
        int batch = 0;
        for (int b = 0; b < live; b++)
        {
            kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_mtx, CH_FULL}}; // done@1, mtx@2
            int t = kos::thread::spawn_caps(churner, nullptr, "churn", 10, ccaps, 2,
                                            KOS_POLICY_FIFO, 0, /*privileged=*/true);
            if (t < 0)
            {
                break;
            }
            batch++;
        }
        for (int b = 0; b < batch; b++)
        {
            kos_sem_wait(g_done); // join only what this batch actually spawned
        }
        if (batch != live)
        {
            churn_ok = false; // a spawn failed mid-batch: reclamation did not free a slot
        }
    }

    long exp_naps = static_cast<long>(sleepers) * NAPS;
    long exp_hand = static_cast<long>(2 * pairs) * ROUNDS;
    long exp_churn = static_cast<long>(live) * CHURN_GENERATIONS;
    int fails = 0;
    if (g_naps_done != exp_naps)
    {
        fails++;
    }
    if (g_handoffs != exp_hand)
    {
        fails++;
    }
    if (not churn_ok or g_churn_runs != exp_churn)
    {
        fails++;
    }

    // Reclaim this round's per-pair sems (workers are all joined -> no user left).
    for (int i = 0; i < made_pairs; i++)
    {
        kos_sem_destroy(g_pair_a[i]);
        kos_sem_destroy(g_pair_b[i]);
    }
    return fails;
}

int main(int, char**)
{
    kos::print("stress: scheduler + semaphore + tickless-timer conservation test\n");

    g_done = kos_sem_create(0);
    g_mtx = kos_sem_create(1);
    // Every create/spawn is checked: a board with a smaller thread/sem pool than
    // this soak needs must SKIP cleanly, not hang a join on a thread that was never
    // created or race on a counter whose mutex silently failed to allocate.
    bool ok = (g_done >= 0 and g_mtx >= 0);

    // Size the soak to this board's pool: probe the concurrent budget, then shrink
    // the (large-pool) footprint until the live set fits. The sim (budget 16) keeps
    // 3 pairs + 6 sleepers unchanged; XMC (budget 8) lands on 3 pairs + 2 sleepers.
    int budget = 0;
    if (ok)
    {
        budget = probe_budget();
    }
    int pairs = MAX_PAIRS;
    int sleepers = MAX_SLEEPERS;
    while (2 * pairs + sleepers > budget and sleepers > 0)
    {
        sleepers--;
    }
    while (2 * pairs + sleepers > budget and pairs > 1)
    {
        pairs--;
    }
    int live = 2 * pairs + sleepers; // conservation footprint == churn batch
    if (not ok or budget < 2 or live > budget)
    {
        kos::print("STRESS SKIP (board thread/sem pool too small)\n# stress complete\n");
        return 0;
    }

#ifdef KICKOS_STRESS_FOREVER
    // Endurance soak: repeat the round forever. A clean round prints a compact
    // heartbeat; the first failing round prints it and FREEZES so the FAIL is the
    // last thing on the wire and the iter counter stops advancing (a silent hang
    // shows the same frozen counter). The freeze reads a volatile so the empty loop
    // is not elided.
    for (long iter = 1;; iter++)
    {
        int fails = run_stress_round(pairs, sleepers, live);
        char s[200];
        if (fails == 0)
        {
            ksnprintf(s, sizeof(s), "[soak] iter %ld PASS  naps %ld handoffs %ld churn %ld\n",
                      iter, g_naps_done, g_handoffs, g_churn_runs);
            kos::print(s);
        }
        else
        {
            ksnprintf(s, sizeof(s), "[soak] iter %ld STRESS FAIL (%d)\n# soak halted\n", iter, fails);
            kos::print(s);
            volatile bool halt = true;
            while (halt)
            {
            }
        }
    }
    return 0; // unreachable
#else
    int fails = run_stress_round(pairs, sleepers, live);

    long exp_naps = static_cast<long>(sleepers) * NAPS;
    long exp_hand = static_cast<long>(2 * pairs) * ROUNDS;
    long exp_churn = static_cast<long>(live) * CHURN_GENERATIONS;
    char s[200];
    ksnprintf(s, sizeof(s), "  naps %ld/%ld  handoffs %ld/%ld  churn %ld/%ld\n",
              g_naps_done, exp_naps, g_handoffs, exp_hand, g_churn_runs, exp_churn);
    kos::print(s);
    if (fails == 0)
    {
        kos::print("STRESS PASS\n# stress complete\n");
    }
    else
    {
        kos::print("STRESS FAIL\n# stress complete\n");
    }
    return fails;
#endif
}
