// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Concurrency stress test (bounded, self-verifying). Hammers the three subsystems
// that carry the subtle races (the scheduler, the counting semaphores, and the
// tickless timer) with many threads interleaving at mixed priorities, then
// asserts exact conservation counts. A lost wakeup, a dropped timer deadline, or a
// wait-queue corruption shows up as either a hang (the join never completes -> the
// harness times out) or a final count mismatch (STRESS FAIL). Terminates cleanly
// (every worker exits, main verifies and returns its failure count), so it doubles as a
// CTest gate. "# stress complete" marks the end of the run, PASS, FAIL or SKIP alike;
// the verdict is the STRESS PASS line, which is what the registered ctest keys on.
//
// Pool use scales to the board: the soak first PROBES the concurrent thread budget
// (spawn parked threads until one is refused), then sizes the ping-pong pairs and
// sleepers to fit it, so it runs a real soak on ANY pool instead of SKIPping the small
// boards. The probed budget is main's THREADS budget less its own slot at most. The printed
// counts are NOT fixed: they shrink from
// MAX_PAIRS/MAX_SLEEPERS with the probed budget, so any knob that moves the thread pool
// moves them. Every create/spawn is still checked: a board too small even for one pair
// SKIPs rather than hanging a join.
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
    constexpr int PROBE_MAX = STRESS_THREADS; // main's THREADS budget; main holds one of its slots
    constexpr int ROUNDS = 3000;      // handoffs per ping-pong thread
    constexpr int NAPS = 100;         // sleeps per sleeper
    constexpr int CHURN_GENERATIONS = 170; // ~2000 spawn/exit cycles total
    // pairs/sleepers/churn-batch are sized at runtime from the probed budget (main).
    constexpr uint64_t NAP_MIN_NS = 80000ull;    // 80 us
    constexpr uint64_t NAP_SPAN_NS = 400000ull;  // + up to 400 us
    constexpr uint64_t LINGER_NS = 20000000ull;  // 20 ms between a worker's g_done post and its exit

    kos_sem_t g_done = KOS_SEM_INIT(0);  // completion counter: each worker posts once as it finishes
    kos_sem_t g_mtx = KOS_SEM_INIT(1);   // binary semaphore guarding the shared counters
    kos_sem_t g_gate = KOS_SEM_INIT(0);  // budget-probe gate: probers park here until released
    kos_sem_t g_start = KOS_SEM_INIT(0); // posted once per pair once every worker exists
    // Each pair's two semaphores, ping and pong naming theirs by the pair's index.
    kos_sem_t g_pair_a[MAX_PAIRS] = {};
    kos_sem_t g_pair_b[MAX_PAIRS] = {};

    // Shared conservation counters (g_mtx serializes every access).
    long g_naps_done = 0;    // total sleeper wakes; expect sleepers*NAPS
    long g_handoffs = 0;     // total ping-pong handoffs; expect 2*pairs*ROUNDS
    long g_churn_runs = 0;   // total churn-worker runs; expect live*CHURN_GENERATIONS

    void lock() { kos_sem_wait(&g_mtx, KOS_TIMEOUT_NONE); }
    void unlock() { kos_sem_post(&g_mtx); }

    // The linger keeps the slot held past the post, so a spawn on the post alone finds the budget
    // short on every board.
    void done()
    {
        kos_sem_post(&g_done);
        kos_sleep_ns(LINGER_NS);
    }

    // Per-thread LCG (seeded off the thread index): deterministic, no shared state.
    uint32_t lcg(uint32_t* s)
    {
        *s = *s * 1103515245u + 12345u;
        return *s;
    }

    // arg = pair index.
    void ping(void* arg)
    {
        int i = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        uint32_t seed = 0x9E3779B9u ^ (static_cast<uint32_t>(i) * 2654435761u);
        kos_sem_t* const a = &g_pair_a[i];
        kos_sem_t* const b = &g_pair_b[i];
        // The kick: the token enters A only once every worker of the round exists.
        kos_sem_wait(&g_start, KOS_TIMEOUT_NONE);
        kos_sem_post(a);
        for (int r = 0; r < ROUNDS; r++)
        {
            kos_sem_wait(a, KOS_TIMEOUT_NONE);
            // Occasional nap: interleave the sem path with the tickless one-shot.
            if ((r & 63) == 0)
            {
                kos_sleep_ns(NAP_MIN_NS + (lcg(&seed) % NAP_SPAN_NS));
            }
            kos_sem_post(b);
            lock();
            g_handoffs++;
            unlock();
        }
        done();
    }

    void pong(void* arg)
    {
        int i = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        kos_sem_t* const a = &g_pair_a[i];
        kos_sem_t* const b = &g_pair_b[i];
        for (int r = 0; r < ROUNDS; r++)
        {
            kos_sem_wait(b, KOS_TIMEOUT_NONE);
            kos_sem_post(a);
            lock();
            g_handoffs++;
            unlock();
        }
        done();
    }

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
        done();
    }

    // Spawn/exit churn worker: bump the counter, signal done, return (the arch
    // trampoline routes the return through exit_current -> EXITED). A non-null arg lingers as
    // done() does.
    void churner(void* arg)
    {
        lock();
        g_churn_runs++;
        unlock();
        if (arg != nullptr)
        {
            done();
        }
        else
        {
            kos_sem_post(&g_done);
        }
    }

    // Budget probe: park on the gate until main releases it, then exit. Spawned
    // until one is refused, so the live count == the board's concurrent thread pool.
    void prober(void*)
    {
        kos_sem_wait(&g_gate, KOS_TIMEOUT_NONE);
        done();
    }

    // The threads of one phase: the probers, the conservation set or a churn batch.
    kos::thread::Handle g_spawned[PROBE_MAX];

    // A g_done post comes before the poster's exit frees its slot, so each thread is also joined
    // by handle before its slot is needed again.
    void join_spawned(int n)
    {
        for (int i = 0; i < n; i++)
        {
            (void)g_spawned[i].join();
        }
    }

    // Concurrent thread budget = how many prober threads can be live at once: main's THREADS
    // budget less main itself, whatever other tasks hold. Spawn until refused, then release +
    // join them all, leaving the budget whole again.
    int probe_budget()
    {
        int n = 0;
        while (n < PROBE_MAX)
        {
            auto t = kos::thread::create(prober, nullptr, "probe", 4);
            if (not t.valid())
            {
                break;
            }
            g_spawned[n] = t;
            n++;
        }
        for (int i = 0; i < n; i++)
        {
            kos_sem_post(&g_gate); // release every parked prober
        }
        for (int i = 0; i < n; i++)
        {
            kos_sem_wait(&g_done, KOS_TIMEOUT_NONE);
        }
        join_spawned(n);
        return n;
    }
}

// One full conservation round, repeatable: resets the accumulators and the per-pair
// semaphores, spawns + kicks + joins the conservation set, runs the churn phase and checks
// exact conservation. g_done/g_mtx/g_start are NOT reset here (they self-balance: every
// worker posts g_done once and is joined; g_mtx is released as often as taken; g_start is
// posted once per ping, which takes it once). Returns the failure count for this round.
int run_stress_round(int pairs, int sleepers, int live)
{
    g_naps_done = 0;
    g_handoffs = 0;
    g_churn_runs = 0;

    bool ok = true;
    int spawned = 0;

    // Mixed priorities straddling the sleepers' band; the last pair is RR.
    for (int i = 0; ok and i < pairs; i++)
    {
        (void)kos_sem_init(&g_pair_a[i], 0);
        (void)kos_sem_init(&g_pair_b[i], 0);
        uint8_t prio = static_cast<uint8_t>(8 + i);
        uint8_t policy = KOS_POLICY_FIFO;
        uint32_t quantum = 0;
        if (i == pairs - 1)
        {
            policy = KOS_POLICY_RR;
            quantum = 300000u; // 300 us
        }
        auto a = kos::thread::create(ping, reinterpret_cast<void*>(uintptr_t(i)), "ping",
                                     prio, policy, quantum);
        auto b = kos::thread::create(pong, reinterpret_cast<void*>(uintptr_t(i)), "pong",
                                     prio, policy, quantum);
        if (not a.valid() or not b.valid())
        {
            ok = false;
            break;
        }
        g_spawned[spawned] = a;
        g_spawned[spawned + 1] = b;
        spawned += 2;
    }
    for (int i = 0; ok and i < sleepers; i++)
    {
        uint8_t prio = static_cast<uint8_t>(6 + (i % 6)); // 6..11, straddling the pairs
        auto t = kos::thread::create(sleeper, reinterpret_cast<void*>(uintptr_t(i)), "sleep",
                                     prio);
        if (not t.valid())
        {
            ok = false;
            break;
        }
        g_spawned[spawned] = t;
        spawned++;
    }

    // A mid-round create/spawn failure means the pool did not return to its start
    // state (a leak): a hard FAIL, not a SKIP: main already proved the budget fits
    // on entry. Do NOT join a half-spawned set (a lone ping would hang the join), and
    // leave every spawned ping parked on the start gate; report the failure so the soak
    // halts on it.
    if (not ok or spawned != live)
    {
        return 1;
    }

    // Kick each pair once; the token then circulates ROUNDS times per side.
    for (int i = 0; i < pairs; i++)
    {
        kos_sem_post(&g_start);
    }

    // Join every worker. A lost wakeup / dropped deadline hangs here -> the harness
    // timeout catches it as a failure.
    for (int i = 0; i < spawned; i++)
    {
        kos_sem_wait(&g_done, KOS_TIMEOUT_NONE);
    }
    join_spawned(spawned);

    // Churn phase: the conservation workers above are all EXITED now, so their pool
    // slots must be reclaimed here: live*CHURN_GENERATIONS >> the pool. A spawn
    // returning -1 means reclamation failed (pool exhausted): a real FAIL, not a
    // SKIP. Each batch is joined before the next spawns, so at most `live` are ever
    // concurrent and the join never waits on a thread that was not created.
    bool churn_ok = true;
    for (int g = 0; churn_ok and g < CHURN_GENERATIONS; g++)
    {
        int batch = 0;
        for (int b = 0; b < live; b++)
        {
            // Two batches: after a lone one, a churner that preempts main to its exit frees a
            // slot for each spawn.
            void* linger = nullptr;
            if (g < 2)
            {
                linger = &g_done;
            }
            auto t = kos::thread::create(churner, linger, "churn", 10);
            if (not t.valid())
            {
                break;
            }
            g_spawned[batch] = t;
            batch++;
        }
        for (int b = 0; b < batch; b++)
        {
            kos_sem_wait(&g_done, KOS_TIMEOUT_NONE); // join only what this batch actually spawned
        }
        join_spawned(batch);
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
    return fails;
}

int main(int, char**)
{
    kos::print("stress: scheduler + semaphore + tickless-timer conservation test\n");

    // Size the soak to this board's pool: probe the concurrent budget, then shrink the
    // footprint until the live set fits. Shrink order matters: sleepers go first, then
    // pairs, so a small board keeps the ping-pong handoff coverage it is here for. A board too
    // small even for that SKIPs rather than hang a join on a thread never created.
    int const budget = probe_budget();
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
    if (budget < 2 or live > budget)
    {
        kos::print("STRESS SKIP (board thread pool too small)\n# stress complete\n");
        return 0;
    }

    // The realized runnable count, which a gate resting on more runnable threads than cores
    // must read: the budget probe above shrinks the workload independently of MAX_PAIRS.
    {
        char r[64];
        ksnprintf(r, sizeof(r), "stress: runnable %d\n", live);
        kos::print(r);
    }

#ifdef KICKOS_STRESS_FOREVER
    // Endurance soak: repeat the round forever. A clean round prints a compact
    // heartbeat; the first failing round prints it and FREEZES so the FAIL is the
    // last thing on the wire and the iter counter stops advancing (a silent hang
    // shows the same frozen counter).
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
            volatile bool halt = true; // optimization barrier: the empty loop must not be elided
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
