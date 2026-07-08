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

    int g_done = -1; // completion counter (each worker posts once at exit)
    int g_mtx = -1;  // binary sem guarding the shared counters
    int g_gate = -1; // budget-probe gate (probers park here until released)

    int g_pair_a[MAX_PAIRS];   // "ping waits A, posts B"
    int g_pair_b[MAX_PAIRS];

    // Shared conservation counters (g_mtx serializes every access).
    long g_naps_done = 0;    // total sleeper wakes; expect sleepers*NAPS
    long g_handoffs = 0;     // total ping-pong handoffs; expect 2*pairs*ROUNDS
    long g_churn_runs = 0;   // total churn-worker runs; expect live*CHURN_GENERATIONS

    void lock() { kos_sem_wait(g_mtx); }
    void unlock() { kos_sem_post(g_mtx); }

    // Per-thread LCG (seeded off the thread index): deterministic, no shared state.
    uint32_t lcg(uint32_t* s)
    {
        *s = *s * 1103515245u + 12345u;
        return *s;
    }

    void ping(void* arg)
    {
        int i = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        uint32_t seed = 0x9E3779B9u ^ (static_cast<uint32_t>(i) * 2654435761u);
        for (int r = 0; r < ROUNDS; r++)
        {
            kos_sem_wait(g_pair_a[i]);
            // Occasional nap: interleave the sem path with the tickless one-shot.
            if ((r & 63) == 0)
            {
                kos_sleep_ns(NAP_MIN_NS + (lcg(&seed) % NAP_SPAN_NS));
            }
            kos_sem_post(g_pair_b[i]);
            lock();
            g_handoffs++;
            unlock();
        }
        kos_sem_post(g_done);
    }

    void pong(void* arg)
    {
        int i = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        for (int r = 0; r < ROUNDS; r++)
        {
            kos_sem_wait(g_pair_b[i]);
            kos_sem_post(g_pair_a[i]);
            lock();
            g_handoffs++;
            unlock();
        }
        kos_sem_post(g_done);
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
        kos_sem_post(g_done);
    }

    // Spawn/exit churn worker: bump the counter, signal done, return (the arch
    // trampoline routes the return through exit_current -> EXITED). MUST run above
    // the driver's priority (root = KICKOS_PRIO_MIN+1): that is the guarantee it
    // reaches EXITED before the driver is rescheduled to spawn the next batch, so its
    // slot is reclaimable in time. Below root, reclaim can miss it -> spurious FAIL.
    void churner(void*)
    {
        lock();
        g_churn_runs++;
        unlock();
        kos_sem_post(g_done);
    }

    // Budget probe: park on the gate until main releases it, then exit. Spawned
    // until one is refused, so the live count == the board's concurrent thread pool.
    void prober(void*)
    {
        kos_sem_wait(g_gate);
        kos_sem_post(g_done);
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
        int n = 0;
        while (n < PROBE_MAX)
        {
            int t = kos::thread::spawn(prober, nullptr, "probe", 4);
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
    int budget = ok ? probe_budget() : 0;
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
        uint8_t prio = static_cast<uint8_t>(8 + i); // 8,9,10
        uint8_t policy = KOS_POLICY_FIFO;
        uint32_t quantum = 0;
        if (i == pairs - 1)
        {
            policy = KOS_POLICY_RR;
            quantum = 300000u; // 300 us
        }
        int a = kos::thread::spawn(ping, reinterpret_cast<void*>(uintptr_t(i)), "ping",
                                   prio, policy, quantum, /*privileged=*/true);
        int b = kos::thread::spawn(pong, reinterpret_cast<void*>(uintptr_t(i)), "pong",
                                   prio, policy, quantum, /*privileged=*/true);
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
        int t = kos::thread::spawn(sleeper, reinterpret_cast<void*>(uintptr_t(i)), "sleep",
                                   prio);
        if (t < 0)
        {
            ok = false;
            break;
        }
        spawned++;
    }
    if (not ok or spawned != live)
    {
        kos::print("STRESS SKIP (board thread/sem pool too small)\n# stress complete\n");
        return 0;
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
            int t = kos::thread::spawn(churner, nullptr, "churn", 10,
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
}
