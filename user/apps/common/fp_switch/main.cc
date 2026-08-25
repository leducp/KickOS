// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// FP context-switch torture test (Cortex-M4F and any FPU armv7m). Proves the
// kernel saves/restores the *callee-saved* FP register bank (s16-s31) across a
// context switch: the half the hardware does NOT auto-stack on exception entry
// (it lazily stacks only s0-s15 + FPSCR; s16-s31 are the PendSV switch's job).
//
// M4F FPU is single-precision, so this uses `float` (s-registers), not `double`
// (which is soft-float on this part and would not touch the FPU registers).

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

namespace
{
#if defined(__aarch64__)
    // ALL THIRTY-TWO, AND BOTH HALVES OF EACH. On M-profile the hardware auto-stacks the
    // caller-saved FP half on exception entry, so the kernel owes only s16-s31 and the arm
    // below is a complete witness there. A64 stacks nothing, so the kernel owes every bit of
    // every register: `d` loads would leave bits 127:64 unseeded and a regression from q
    // saves to d saves would pass. FPCR and FPSR ride along, being thread state too.
    constexpr int N = 64; // 32 registers, two 64-bit halves each

    __attribute__((always_inline)) inline void fp_bank_load(unsigned long long const* in)
    {
        __asm volatile(
            "ldp q0, q1, [%0, #0]\n\t"     "ldp q2, q3, [%0, #32]\n\t"
            "ldp q4, q5, [%0, #64]\n\t"    "ldp q6, q7, [%0, #96]\n\t"
            "ldp q8, q9, [%0, #128]\n\t"   "ldp q10, q11, [%0, #160]\n\t"
            "ldp q12, q13, [%0, #192]\n\t" "ldp q14, q15, [%0, #224]\n\t"
            "ldp q16, q17, [%0, #256]\n\t" "ldp q18, q19, [%0, #288]\n\t"
            "ldp q20, q21, [%0, #320]\n\t" "ldp q22, q23, [%0, #352]\n\t"
            "ldp q24, q25, [%0, #384]\n\t" "ldp q26, q27, [%0, #416]\n\t"
            "ldp q28, q29, [%0, #448]\n\t" "ldp q30, q31, [%0, #480]"
            ::"r"(in)
            : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
              "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
              "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
              "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "memory");
    }
    __attribute__((always_inline)) inline void fp_bank_snapshot(unsigned long long* out)
    {
        __asm volatile(
            "stp q0, q1, [%0, #0]\n\t"     "stp q2, q3, [%0, #32]\n\t"
            "stp q4, q5, [%0, #64]\n\t"    "stp q6, q7, [%0, #96]\n\t"
            "stp q8, q9, [%0, #128]\n\t"   "stp q10, q11, [%0, #160]\n\t"
            "stp q12, q13, [%0, #192]\n\t" "stp q14, q15, [%0, #224]\n\t"
            "stp q16, q17, [%0, #256]\n\t" "stp q18, q19, [%0, #288]\n\t"
            "stp q20, q21, [%0, #320]\n\t" "stp q22, q23, [%0, #352]\n\t"
            "stp q24, q25, [%0, #384]\n\t" "stp q26, q27, [%0, #416]\n\t"
            "stp q28, q29, [%0, #448]\n\t" "stp q30, q31, [%0, #480]"
            ::"r"(out) : "memory");
    }
    // FPCR's RMode field and FPSR's IXC sticky flag: a switch that carried the vector bank
    // and dropped these would pass the bank comparison alone.
    inline unsigned long long fpcr_fpsr_read()
    {
        unsigned long long c = 0;
        unsigned long long t = 0;
        __asm volatile("mrs %0, fpcr" : "=r"(c));
        __asm volatile("mrs %0, fpsr" : "=r"(t));
        return ((c & (3ull << 22)) << 8) | (t & (1ull << 4));
    }
    inline void fpcr_fpsr_trash()
    {
        unsigned long long c = 0;
        unsigned long long t = 0;
        __asm volatile("mrs %0, fpcr" : "=r"(c));
        __asm volatile("mrs %0, fpsr" : "=r"(t));
        c = (c & ~(3ull << 22)) | (1ull << 22); // RMode = toward plus infinity
        t = t & ~(1ull << 4);                   // IXC cleared
        __asm volatile("msr fpcr, %0" ::"r"(c));
        __asm volatile("msr fpsr, %0" ::"r"(t));
    }
    inline void fpcr_fpsr_seed()
    {
        unsigned long long c = 0;
        unsigned long long t = 0;
        __asm volatile("mrs %0, fpcr" : "=r"(c));
        __asm volatile("mrs %0, fpsr" : "=r"(t));
        c = (c & ~(3ull << 22)) | (2ull << 22); // RMode = toward minus infinity
        t = t | (1ull << 4);                    // IXC sticky
        __asm volatile("msr fpcr, %0" ::"r"(c));
        __asm volatile("msr fpsr, %0" ::"r"(t));
    }
#else
    constexpr int N = 16; // s16 .. s31
#endif

#if defined(__aarch64__)
    // The bank is doubles here, so the two helpers above already match the signatures the
    // shared body below uses once `float` is spelled as this alias.
    using fp_word = unsigned long long;
#else
    using fp_word = float;
#endif

#if defined(__ARM_FP) && !defined(__aarch64__)
    // MUST be always-inlined into the caller: s16-s31 are callee-saved, so a real
    // function's epilogue would vpop (restore) them and wipe out the load/hold. By
    // inlining, the bank stays live across the intervening sleep call, and only the
    // *caller's* single prologue/epilogue touches it, never between load and read.
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
#elif !defined(__aarch64__)
    inline void fp_bank_load(fp_word const*) {}
    inline void fp_bank_snapshot(fp_word* out)
    {
        for (int i = 0; i < N; i++)
        {
            out[i] = 0.0;
        }
    }
#endif

    // Lower priority than the checker -> runs whenever the checker is asleep, so
    // it is guaranteed to overwrite s16-s31 during every checker sleep window.
    void trasher(void*)
    {
        fp_word junk[N];
        for (int i = 0; i < N; i++)
        {
            junk[i] = 0x9000000000000000ull + static_cast<fp_word>(i);
        }
        while (true)
        {
            fp_bank_load(junk);
#if defined(__aarch64__)
            // The trasher has to clobber the control registers too, or the checker's seeded
            // values survive by accident and the FPCR/FPSR arm proves nothing.
            fpcr_fpsr_trash();
#endif
            for (volatile int i = 0; i < 2000;)
            {
                i = i + 1;
            }
        }
    }

    void checker(void*)
    {
        fp_word sent[N];
        for (int i = 0; i < N; i++)
        {
            sent[i] = 0x0123456789ab0000ull + static_cast<fp_word>(i);
        }
        int rounds = 0;
        while (true)
        {
            fp_bank_load(sent);
#if defined(__aarch64__)
            fpcr_fpsr_seed();
            unsigned long long const ctl_sent = fpcr_fpsr_read();
#endif
            kos::sleep_ns(100000000ull); // 0.1 s: the trasher runs and clobbers the bank
            fp_word rd[N];
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
#if defined(__aarch64__)
            if (bad < 0 and fpcr_fpsr_read() != ctl_sent)
            {
                kos::print("  FP FAIL: FPCR/FPSR did not survive the switch\n");
                bad = N; // reported below as the control-register arm
            }
            if (bad >= 0)
            {
                ksnprintf(b, sizeof(b), "  FP FAIL: v%d half %d (round %d)\n",
                          bad / 2, bad % 2, rounds);
                kos::print(b);
            }
#else
            if (bad >= 0)
            {
                ksnprintf(b, sizeof(b), "  FP FAIL: s%d = %d, expected %d (round %d)\n",
                          16 + bad, static_cast<int>(rd[bad]), static_cast<int>(sent[bad]), rounds);
                kos::print(b);
            }
#endif
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
    kos::print("  (no hardware FPU on this target; nothing to test)\n");
#endif

    kos::thread::spawn(checker, nullptr, "checker", 20);
    kos::thread::spawn(trasher, nullptr, "trasher", 5);

    kos::Semaphore idle(0);
    while (true)
    {
        idle.wait();
    }
}
