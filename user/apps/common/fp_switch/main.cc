// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// FP context-switch torture test (Cortex-M4F and any FPU armv7m). Proves the
// kernel saves/restores the *callee-saved* FP register bank (s16-s31) across a
// context switch -- the half the hardware does NOT auto-stack on exception entry
// (it lazily stacks only s0-s15 + FPSCR; s16-s31 are the PendSV switch's job).
//
// A high-priority CHECKER loads a known sentinel pattern into s16-s31, sleeps
// (yielding the CPU), then reads s16-s31 back and verifies the pattern survived.
// A low-priority TRASHER runs only while the checker sleeps and continuously
// loads a DIFFERENT pattern into s16-s31. If the switch does not bank s16-s31,
// the checker wakes to the trasher's values and prints "FP FAIL" loudly.
//
// M4F FPU is single-precision, so this uses `float` (s-registers), not `double`
// (which is soft-float on this part and would not touch the FPU registers).
//
// Watch the console: repeated "FP OK ... s16-s31 preserved" = pass; any
// "FP FAIL" = the FP-switch path is broken on this target.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

namespace
{
    constexpr int N = 16; // s16 .. s31

#if defined(__ARM_FP)
    // MUST be always-inlined into the caller: s16-s31 are callee-saved, so a real
    // function's epilogue would vpop (restore) them and wipe out the load/hold. By
    // inlining, the bank stays live across the intervening sleep call, and only the
    // *caller's* single prologue/epilogue touches it -- never between load and read.
    __attribute__((always_inline)) inline void fp_bank_load(float const* in)
    {
        __asm volatile("vldmia %0, {s16-s31}" ::"r"(in)
                       : "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
                         "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31", "memory");
    }
    __attribute__((always_inline)) inline void fp_bank_snapshot(float* out)
    {
        __asm volatile("vstmia %0, {s16-s31}" ::"r"(out) : "memory");
    }
#else
    inline void fp_bank_load(float const*) {}
    inline void fp_bank_snapshot(float* out)
    {
        for (int i = 0; i < N; i++)
        {
            out[i] = 0.0f;
        }
    }
#endif

    // Higher priority than the checker -> runs whenever the checker is asleep, so
    // it is guaranteed to overwrite s16-s31 during every checker sleep window.
    void trasher(void*)
    {
        float junk[N];
        for (int i = 0; i < N; i++)
        {
            junk[i] = 900.0f + static_cast<float>(i);
        }
        while (true)
        {
            fp_bank_load(junk);
            for (volatile int i = 0; i < 2000; i++)
            {
            }
        }
    }

    void checker(void*)
    {
        float sent[N];
        for (int i = 0; i < N; i++)
        {
            sent[i] = 100.0f + static_cast<float>(i);
        }
        int rounds = 0;
        while (true)
        {
            fp_bank_load(sent);
            kos::sleep_ns(100000000ull); // 0.1 s: the trasher runs + clobbers s16-s31
            float rd[N];
            fp_bank_snapshot(rd);

            int bad = -1;
            for (int i = 0; i < N; i++)
            {
                if (rd[i] != sent[i])
                {
                    bad = i;
                    break;
                }
            }
            char b[96];
            if (bad >= 0)
            {
                ksnprintf(b, sizeof(b), "  FP FAIL: s%d = %d, expected %d (round %d)\n",
                          16 + bad, static_cast<int>(rd[bad]), static_cast<int>(sent[bad]), rounds);
                kos::print(b);
            }
            else
            {
                rounds++;
                if ((rounds % 10) == 0)
                {
                    ksnprintf(b, sizeof(b), "  FP OK: %d rounds, s16-s31 preserved across switch\n",
                              rounds);
                    kos::print(b);
                }
            }
        }
    }
}

int main(int, char**)
{
    kos::print("FP switch test: checker verifies s16-s31 survive a switch while\n");
    kos::print("a lower-priority trasher clobbers them. Result lines follow.\n\n");

#if !defined(__ARM_FP)
    kos::print("  (no hardware FPU on this target -- nothing to test)\n");
#endif

    kos::thread::spawn(checker, nullptr, "checker", 20);
    kos::thread::spawn(trasher, nullptr, "trasher", 5);

    kos::Semaphore idle(0);
    while (true)
    {
        idle.wait();
    }
}
