// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Clock-hardening SILICON soak harness (M3 clock off the debug domain -> wide
// peripheral monotonic timer). Gated OFF by default (-DKICKOS_CLOCKSOAK_TEST=ON).
// It answers the "silicon-test-later" checklist that no emulator reaches:
//   1. idle-wrap observer: idle past >1 counter wrap period -> clock still correct
//      (the wrap folds in the timer overflow ISR, not by a thread read).
//   2. soak across several wraps -> no phantom 2^32-style leap, no backward stall.
//   3. rate/monotonicity vs the requested sleep (a 2x error == wrong Hz).
//   4. the idle path (WFI/WAITI) keeps the counter running (a frozen counter would
//      make every measured chunk read short or strand).
// PASS is operator-observable on the wire (raw prints + an explicit verdict line).
//
// The wrap period is per board (2^32/timer_hz for the 32-bit v7-M timers; the 64-bit
// TIMG0/CCU4/PIT/CLINT boards never wrap in practice, so the soak degrades to a pure
// rate/monotonicity + idle-keeps-counting run). Set it with -DKICKOS_CLOCKSOAK_WRAP_MS
// (default 60000): F411 TIM2 ~= 51 s, F302 ~= 67 s, F103 chain ~= 59 s, SAM3X TC0 ~= 102 s.
//
// Runs PRIVILEGED on the kernel root thread (kickos_app_main), single-shot: it returns,
// so root_entry flushes the console before shutdown. Uses only clock_now/sleep_ns.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

#ifndef KICKOS_CLOCKSOAK_WRAP_MS
#define KICKOS_CLOCKSOAK_WRAP_MS 60000u
#endif

#ifndef KICKOS_CLOCKSOAK_WRAPS
#define KICKOS_CLOCKSOAK_WRAPS 3u
#endif

namespace
{
    constexpr uint64_t WRAP_NS = static_cast<uint64_t>(KICKOS_CLOCKSOAK_WRAP_MS) * 1000000ull;
    constexpr uint32_t WRAPS = KICKOS_CLOCKSOAK_WRAPS;

    void emit(char const* s)
    {
        kos::print(s);
    }

    // A measured chunk must land at or just above the requested sleep. Short == a lost
    // wrap (the counter froze or a wrap was dropped); a large excess == a phantom leap
    // (the DWT-style +2^32 that this whole fix exists to kill). Window is deliberately
    // wide (tickless granularity + wake latency add a little on top).
    bool chunk_ok(uint64_t requested, uint64_t measured)
    {
        uint64_t const lo = (requested * 97ull) / 100ull;  // 3% short-tolerance floor
        uint64_t const hi = (requested * 110ull) / 100ull; // 10% over-tolerance ceiling
        return (measured >= lo) and (measured <= hi);
    }
}

int main(int, char**)
{
    char s[192];
    bool pass = true;

    emit("[clocksoak] START clock-hardening soak harness\n");

    uint32_t const hz = kos::cpu_clock_hz();
    uint64_t const t0 = kos::clock_now();
    ksnprintf(s, sizeof(s),
              "[clocksoak] boot: cpu_clock_hz = %u  wrap_period = %u ms  wraps = %u  t0 = %llu ns\n",
              static_cast<unsigned>(hz), static_cast<unsigned>(KICKOS_CLOCKSOAK_WRAP_MS),
              static_cast<unsigned>(WRAPS), static_cast<unsigned long long>(t0));
    emit(s);

    // Phase A -- idle past ONE full wrap in a single sleep. The free-running counter
    // wraps mid-sleep with no thread reading it, so the wrap MUST be folded by the
    // overflow ISR for the sleep to resolve at the right wall time (checklist 1/4/5).
    uint64_t const a_req = WRAP_NS + (WRAP_NS / 2ull); // 1.5x wrap
    emit("[clocksoak] phase A: single sleep past 1 wrap (idle-wrap observer)\n");
    uint64_t const a0 = kos::clock_now();
    kos::sleep_ns(a_req);
    uint64_t const a1 = kos::clock_now();
    uint64_t const a_meas = a1 - a0;
    bool const a_mono = a1 >= a0;
    bool const a_rate = chunk_ok(a_req, a_meas);
    ksnprintf(s, sizeof(s),
              "[clocksoak] phase A: requested %llu ns, measured %llu ns  (mono=%u rate=%u)\n",
              static_cast<unsigned long long>(a_req), static_cast<unsigned long long>(a_meas),
              static_cast<unsigned>(a_mono), static_cast<unsigned>(a_rate));
    emit(s);
    if (not a_mono)
    {
        emit("[clocksoak] phase A FAIL: backward step across the wrap\n");
        pass = false;
    }
    if (not a_rate)
    {
        emit("[clocksoak] phase A FAIL: lost wrap (short) or phantom leap (long)\n");
        pass = false;
    }

    // Phase B -- soak across several wraps back to back; assert per-chunk rate and
    // seam-monotonicity (a burst of reads between chunks must never step backward).
    emit("[clocksoak] phase B: soak across N wraps\n");
    uint64_t prev = kos::clock_now();
    uint64_t soak_req = 0;
    for (uint32_t i = 0; i < WRAPS; i++)
    {
        uint64_t const b0 = kos::clock_now();
        kos::sleep_ns(WRAP_NS);
        uint64_t const b1 = kos::clock_now();

        // Seam reads: rapid, must be monotonic with no backward step at the boundary.
        uint64_t const s0 = kos::clock_now();
        uint64_t const s1 = kos::clock_now();
        uint64_t const s2 = kos::clock_now();
        bool const seam_mono = (b1 >= b0) and (s0 >= b1) and (s1 >= s0) and (s2 >= s1);

        uint64_t const meas = b1 - b0;
        soak_req += WRAP_NS;
        bool const rate = chunk_ok(WRAP_NS, meas);
        ksnprintf(s, sizeof(s),
                  "[clocksoak] wrap %u/%u: measured %llu ns  cum=%llu ns  (seam_mono=%u rate=%u)\n",
                  static_cast<unsigned>(i + 1), static_cast<unsigned>(WRAPS),
                  static_cast<unsigned long long>(meas),
                  static_cast<unsigned long long>(s2 - t0),
                  static_cast<unsigned>(seam_mono), static_cast<unsigned>(rate));
        emit(s);
        if (not seam_mono)
        {
            emit("[clocksoak] phase B FAIL: backward step at a chunk seam\n");
            pass = false;
        }
        if (not rate)
        {
            emit("[clocksoak] phase B FAIL: chunk short (lost wrap) or long (phantom leap)\n");
            pass = false;
        }
        if (s2 < prev)
        {
            emit("[clocksoak] phase B FAIL: non-monotonic across iterations\n");
            pass = false;
        }
        prev = s2;
    }

    // Whole-soak rate: total measured vs total requested, x100. ~100 == correct Hz;
    // a gross deviation == wrong timer frequency (checklist 3).
    uint64_t const soak_meas = prev - (t0 + a_meas);
    if (soak_req != 0)
    {
        uint64_t const ratio100 = (soak_meas * 100ull) / soak_req;
        ksnprintf(s, sizeof(s),
                  "[clocksoak] soak total: requested %llu ns, measured %llu ns, ratio x100 = %llu (expect ~100)\n",
                  static_cast<unsigned long long>(soak_req),
                  static_cast<unsigned long long>(soak_meas),
                  static_cast<unsigned long long>(ratio100));
        emit(s);
    }

    if (pass)
    {
        emit("[clocksoak] VERDICT PASS: monotonic across wraps, rate-correct, idle kept counting\n");
    }
    else
    {
        emit("[clocksoak] VERDICT FAIL: see the FAIL lines above\n");
    }
    emit("[clocksoak] clock soak test done\n");
    return 0;
}
