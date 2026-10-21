// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The switcher's rewind of a user compare-and-swap, counted. Two round-robin threads of one
// priority add to one word through kickos_user_cas on one core, sliced at one clock granule, and
// the rewind witness's kernel answers how many times its switcher moved a stacked PC inside the
// routine's window back to its load. The run passes when that happened at least once and the
// word equals the adds both threads saw succeed: a switcher that skips the rewind counts none,
// and a store it resumes lands over the peer's adds.

#include <kickos/arch/arch.h> // arch_syscall
#include <kickos/kos.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/user_cas.h>
#include <kickos/sys/witness.h>

#include <kickos/libc/fmt.h>

#include <stdint.h>

#if not KICKOS_WITNESS_REWIND
#error "ucasrewind runs on the rewind witness's kernel alone"
#endif

namespace
{
    constexpr uint64_t RUN_NS = 100000000ull;
    // Below main, so neither adder runs before both are spawned.
    constexpr uint8_t ADDER_PRIO = 1;

    uint32_t g_word = 0;
    volatile uint64_t g_run_ns = 0;
    volatile uint32_t g_adds[2] = {0, 0};
    volatile uint32_t g_misses[2] = {0, 0};

    uint64_t clock_granule_ns()
    {
        uint64_t const e0 = kos_clock_now();
        uint64_t e1 = e0;
        while (e1 == e0)
        {
            e1 = kos_clock_now();
        }
        uint64_t e2 = e1;
        while (e2 == e1)
        {
            e2 = kos_clock_now();
        }
        return e2 - e1;
    }

    int64_t rewinds()
    {
        return static_cast<int64_t>(
            static_cast<intptr_t>(arch_syscall(KOS_SYS_WITNESS_REWINDS, 0, 0, 0, 0)));
    }

    void adder(void* arg)
    {
        uintptr_t const me = reinterpret_cast<uintptr_t>(arg);
        uint64_t const start = kos_clock_now();
        uint32_t adds = 0;
        uint32_t misses = 0;
        uint32_t v = 0;
        uint32_t i = 0;
        while (true)
        {
            uint32_t const got = kickos_user_cas(&g_word, v, v + 1u);
            if (got == v)
            {
                adds = adds + 1u;
                v = v + 1u;
            }
            else
            {
                misses = misses + 1u;
                v = got;
            }
            i = i + 1u;
            // The clock rarely: a read is a kernel entry, and the switches must land in the CAS.
            if ((i & 1023u) == 0u and kos_clock_now() - start >= g_run_ns)
            {
                break;
            }
        }
        g_adds[me] = adds;
        g_misses[me] = misses;
    }
}

int main(int, char**)
{
    uint64_t const quantum = clock_granule_ns();
    uint64_t run = RUN_NS;
    if (run < quantum * 64u)
    {
        run = quantum * 64u;
    }
    g_run_ns = run;
    char msg[160];
    int64_t const before = rewinds();
    if (before < 0)
    {
        ksnprintf(msg, sizeof(msg), "[rewind] ERROR: the kernel answered %lld for its count\n",
                  static_cast<long long>(before));
        kos_print(msg);
        return 1;
    }
    kos::thread::Handle a =
        kos::thread::create(adder, reinterpret_cast<void*>(uintptr_t{0}), "rwA", ADDER_PRIO,
                            KOS_POLICY_RR, static_cast<uint32_t>(quantum));
    kos::thread::Handle b =
        kos::thread::create(adder, reinterpret_cast<void*>(uintptr_t{1}), "rwB", ADDER_PRIO,
                            KOS_POLICY_RR, static_cast<uint32_t>(quantum));
    if (not a.valid() or not b.valid())
    {
        kos_print("[rewind] ERROR: an adder did not spawn\n");
        return 1;
    }
    (void)a.join(KOS_TIMEOUT_NONE);
    (void)b.join(KOS_TIMEOUT_NONE);
    int64_t const moved = rewinds() - before;
    uint32_t const adds = g_adds[0] + g_adds[1];
    ksnprintf(msg, sizeof(msg),
              "[rewind] quantum %u ns, adds %u + %u, failed compares %u + %u, word %u, "
              "rewinds %lld\n",
              static_cast<unsigned>(quantum), static_cast<unsigned>(g_adds[0]),
              static_cast<unsigned>(g_adds[1]), static_cast<unsigned>(g_misses[0]),
              static_cast<unsigned>(g_misses[1]), static_cast<unsigned>(g_word),
              static_cast<long long>(moved));
    kos_print(msg);
    if (moved < 1)
    {
        kos_print("[rewind] ERROR: no switch moved a thread back to the load\n");
        return 1;
    }
    if (g_word != adds)
    {
        kos_print("[rewind] ERROR: the word fell short of the adds both threads saw succeed\n");
        return 1;
    }
    kos_print("REWIND PASS\n");
    return 0;
}
