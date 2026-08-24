// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// PRIVILEGED runtime clock-retune silicon harness (M3 clock-select coherence tail).
// Gated OFF by default (-DKICKOS_CLOCK_RETUNE_TEST=ON); XMC4800 / K64F only, both of
// which strong-override arch_cpu_clock_set. It drives the privileged
// mask/disarm/flush-to-shift-idle/re-anchor/baud/re-arm sequence on silicon
// (kernel/time/clock_select.cc, section 2.3 of docs/design-m3-clock-select.md); the
// selftest cpu_clock_set case covers the UNPRIVILEGED returns-0 path.
//
// kos_cpu_clock_set needs AUTH_PSTATE, carried only by this app's KICKOS_APP_AUTHORITY
// below. It returns 0 rather than an errno when refused. The app is single-shot: it
// returns, so the terminal path flushes the console synchronously: every printed byte
// reaches the wire.
//
// The console must stay KERNEL_OWNED throughout: a retune is REFUSED while the console
// is USER_OWNED (S4).

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

namespace
{
    // A few million loop iterations of a non-elidable nop: asm volatile has a side
    // effect, so the compiler keeps the loop. Same fixed count at MAX and at LOW: the
    // measured wall time must scale with the clock ratio.
    constexpr uint32_t SPIN_ITERS = 8000000u;

    // The known sleep at LOW: the capture's wall-clock timing must land near this.
    constexpr uint64_t SLEEP_NS = 200000000ull; // 200 ms

    void emit(char const* s)
    {
        kos::print(s);
    }

    void fixed_spin()
    {
        for (uint32_t i = 0; i < SPIN_ITERS; i++)
        {
            __asm volatile("nop" ::: "memory");
        }
    }

    uint64_t timed_spin(char const* label)
    {
        uint64_t const a = kos::clock_now();
        fixed_spin();
        uint64_t const b = kos::clock_now();
        uint64_t const d = b - a;

        char s[128];
        ksnprintf(s, sizeof(s), "[clockretune] spin %s: %u iters in %llu ns\n",
                  label, static_cast<unsigned>(SPIN_ITERS),
                  static_cast<unsigned long long>(d));
        emit(s);
        return d;
    }

    void print_hz(char const* label, uint32_t hz)
    {
        char s[96];
        ksnprintf(s, sizeof(s), "[clockretune] %s: cpu_clock_hz = %u\n", label,
                  static_cast<unsigned>(hz));
        emit(s);
    }
}

KICKOS_APP_AUTHORITY(KOS_AUTH_SYSTEM | KOS_AUTH_PSTATE);

int main(int, char**)
{
    char s[160];

    emit("[clockretune] START privileged retune harness\n");

    uint32_t const hz_boot = kos::cpu_clock_hz();
    uint64_t const t0 = kos::clock_now();
    ksnprintf(s, sizeof(s), "[clockretune] boot: cpu_clock_hz = %u  clock_now t0 = %llu ns\n",
              static_cast<unsigned>(hz_boot), static_cast<unsigned long long>(t0));
    emit(s);

    uint64_t const spin_max = timed_spin("MAX");

    // 1. retune -> LOW. Sample now() immediately BEFORE the seam so step 2 can bound
    //    the mispriced-window delta across the actual PLL/divider move.
    uint64_t const t_pre = kos::clock_now();
    uint32_t const hz_low = kos_cpu_clock_set(KOS_PSTATE_LOW);
    print_hz("after set(LOW)", hz_low);

    // 2. IMMEDIATELY re-read now() several times. Assert monotonic (no backward step)
    //    and no phantom forward jump (a jump == the B1/B2 hazard surviving on silicon:
    //    the seam-crossing delta would be seconds-to-minutes, not the tens-of-us to
    //    ~1 ms masked transition).
    uint64_t const r0 = kos::clock_now();
    uint64_t const r1 = kos::clock_now();
    uint64_t const r2 = kos::clock_now();
    uint64_t const r3 = kos::clock_now();
    ksnprintf(s, sizeof(s), "[clockretune] now around retune: pre=%llu r0=%llu r1=%llu r2=%llu r3=%llu\n",
              static_cast<unsigned long long>(t_pre), static_cast<unsigned long long>(r0),
              static_cast<unsigned long long>(r1), static_cast<unsigned long long>(r2),
              static_cast<unsigned long long>(r3));
    emit(s);

    bool const monotonic = (r0 >= t_pre) and (r1 >= r0) and (r2 >= r1) and (r3 >= r2);
    uint64_t const cross = r0 - t_pre; // seam-crossing delta (only valid if monotonic)
    ksnprintf(s, sizeof(s), "[clockretune] seam-crossing delta r0-pre = %llu ns\n",
              static_cast<unsigned long long>(cross));
    emit(s);
    if (not monotonic)
    {
        emit("[clockretune] MONOTONIC FAIL: backward step across retune\n");
    }
    else if (cross > 100000000ull) // > 100 ms across a few instructions == a phantom jump
    {
        emit("[clockretune] MONOTONIC FAIL: phantom forward jump across retune\n");
    }
    else
    {
        emit("[clockretune] MONOTONIC OK: no backward step, no phantom jump\n");
    }

    // 3. fixed spin AT LOW: same work, must read ~(MAX/LOW ratio) LONGER, proving both
    //    that the clock actually changed AND that now() stayed coherent (same physical
    //    work priced at the new rate).
    uint64_t const spin_low = timed_spin("LOW");
    if (spin_max != 0 and hz_low != 0)
    {
        // Integer ratio x100 (the formatter has no width flag, so no "x.yy"): a
        // measured 300 vs an expected hz_MAX/hz_LOW x100 of 300 is a 3.00x match.
        uint64_t const ratio100 = (spin_low * 100ull) / spin_max;
        uint64_t const expect100 = (static_cast<uint64_t>(hz_boot) * 100ull) / hz_low;
        ksnprintf(s, sizeof(s),
                  "[clockretune] spin ratio LOW/MAX x100 = %llu (expect ~ %llu = hz_MAX/hz_LOW)\n",
                  static_cast<unsigned long long>(ratio100),
                  static_cast<unsigned long long>(expect100));
        emit(s);
    }

    // 4. known sleep at LOW: the capture's wall-clock timing must land near SLEEP_NS
    //    (timer re-armed at the new rate, not ratio-off).
    uint64_t const s0 = kos::clock_now();
    kos::sleep_ns(SLEEP_NS);
    uint64_t const s1 = kos::clock_now();
    ksnprintf(s, sizeof(s),
              "[clockretune] sleep_ns(%llu) at LOW measured %llu ns\n",
              static_cast<unsigned long long>(SLEEP_NS),
              static_cast<unsigned long long>(s1 - s0));
    emit(s);

    // 5. retune -> MAX; console must STILL be readable after both retunes (baud
    //    re-derived at each rate).
    uint32_t const hz_max = kos_cpu_clock_set(KOS_PSTATE_MAX);
    print_hz("after set(MAX)", hz_max);
    emit("[clockretune] console still readable after both retunes (this line proves baud)\n");

    emit("[clockretune] clock retune test done\n");
    return 0;
}
