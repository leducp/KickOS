// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// FP context-switch torture test: a checker seeds the FP state a switch owes, blocks while a
// lower-priority trasher clobbers it, and checks it on wake. What a switch owes is per ISA:
//   - armv7m: s16-s31, the callee-saved half the hardware does not stack on exception entry;
//   - armv8a: all thirty-two vector registers, both halves, and FPCR/FPSR;
//   - x86_64: the sixteen xmm registers and MXCSR;
//   - lx6: FCR and FSR across a block, and f0-f15 across a preemption.
//
// A caller-saved register is the callee's to clobber, so where the bank is caller-saved the
// load, the blocking system call and the snapshot are ONE asm statement: nothing but the
// kernel runs between them.
//
// M4F FPU is single-precision, so the armv7m bank is `float` (s-registers), not `double`
// (which is soft-float on this part and would not touch the FPU registers).

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

namespace
{
    constexpr uint32_t BLOCK_NS = 100000000u; // 0.1 s: the trasher runs and clobbers the state

#if defined(__aarch64__)
    constexpr bool HAS_BANK = true;
    constexpr bool HAS_CTL = true;
    constexpr int N = 64; // 32 registers, two 64-bit halves each
    using fp_word = unsigned long long;

    __attribute__((always_inline)) inline void fp_bank_load(fp_word const* in)
    {
        __asm volatile(
            "ldp q0, q1, [%[in], #0]\n\t"
            "ldp q2, q3, [%[in], #32]\n\t"
            "ldp q4, q5, [%[in], #64]\n\t"
            "ldp q6, q7, [%[in], #96]\n\t"
            "ldp q8, q9, [%[in], #128]\n\t"
            "ldp q10, q11, [%[in], #160]\n\t"
            "ldp q12, q13, [%[in], #192]\n\t"
            "ldp q14, q15, [%[in], #224]\n\t"
            "ldp q16, q17, [%[in], #256]\n\t"
            "ldp q18, q19, [%[in], #288]\n\t"
            "ldp q20, q21, [%[in], #320]\n\t"
            "ldp q22, q23, [%[in], #352]\n\t"
            "ldp q24, q25, [%[in], #384]\n\t"
            "ldp q26, q27, [%[in], #416]\n\t"
            "ldp q28, q29, [%[in], #448]\n\t"
            "ldp q30, q31, [%[in], #480]\n\t"
            ::[in] "r"(in)
            : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11",
              "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22",
              "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "memory");
    }
    void fp_bank_cycle(fp_word const* in, fp_word* out)
    {
        register uintptr_t x0 __asm("x0") = KOS_SYS_SLEEP_NS;
        register uintptr_t x1 __asm("x1") = BLOCK_NS;
        register uintptr_t x2 __asm("x2") = 0;
        register uintptr_t x3 __asm("x3") = 0;
        register uintptr_t x4 __asm("x4") = 0;
        __asm volatile(
            "ldp q0, q1, [%[in], #0]\n\t"
            "ldp q2, q3, [%[in], #32]\n\t"
            "ldp q4, q5, [%[in], #64]\n\t"
            "ldp q6, q7, [%[in], #96]\n\t"
            "ldp q8, q9, [%[in], #128]\n\t"
            "ldp q10, q11, [%[in], #160]\n\t"
            "ldp q12, q13, [%[in], #192]\n\t"
            "ldp q14, q15, [%[in], #224]\n\t"
            "ldp q16, q17, [%[in], #256]\n\t"
            "ldp q18, q19, [%[in], #288]\n\t"
            "ldp q20, q21, [%[in], #320]\n\t"
            "ldp q22, q23, [%[in], #352]\n\t"
            "ldp q24, q25, [%[in], #384]\n\t"
            "ldp q26, q27, [%[in], #416]\n\t"
            "ldp q28, q29, [%[in], #448]\n\t"
            "ldp q30, q31, [%[in], #480]\n\t"
            "svc #0\n\t"
            "stp q0, q1, [%[out], #0]\n\t"
            "stp q2, q3, [%[out], #32]\n\t"
            "stp q4, q5, [%[out], #64]\n\t"
            "stp q6, q7, [%[out], #96]\n\t"
            "stp q8, q9, [%[out], #128]\n\t"
            "stp q10, q11, [%[out], #160]\n\t"
            "stp q12, q13, [%[out], #192]\n\t"
            "stp q14, q15, [%[out], #224]\n\t"
            "stp q16, q17, [%[out], #256]\n\t"
            "stp q18, q19, [%[out], #288]\n\t"
            "stp q20, q21, [%[out], #320]\n\t"
            "stp q22, q23, [%[out], #352]\n\t"
            "stp q24, q25, [%[out], #384]\n\t"
            "stp q26, q27, [%[out], #416]\n\t"
            "stp q28, q29, [%[out], #448]\n\t"
            "stp q30, q31, [%[out], #480]\n\t"
            : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4)
            : [in] "r"(in), [out] "r"(out)
            : "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
              "x17", "x18", "x30",
              "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11",
              "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22",
              "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "cc", "memory");
    }

    // FPCR's RMode field and FPSR's IXC sticky flag.
    inline unsigned long long fp_ctl_read()
    {
        unsigned long long c = 0;
        unsigned long long t = 0;
        __asm volatile("mrs %0, fpcr" : "=r"(c));
        __asm volatile("mrs %0, fpsr" : "=r"(t));
        return ((c & (3ull << 22)) << 8) | (t & (1ull << 4));
    }
    inline void fp_ctl_write(unsigned rmode, bool inexact)
    {
        unsigned long long c = 0;
        unsigned long long t = 0;
        __asm volatile("mrs %0, fpcr" : "=r"(c));
        __asm volatile("mrs %0, fpsr" : "=r"(t));
        c = (c & ~(3ull << 22)) | (static_cast<unsigned long long>(rmode) << 22);
        t = t & ~(1ull << 4);
        if (inexact)
        {
            t = t | (1ull << 4);
        }
        __asm volatile("msr fpcr, %0" ::"r"(c));
        __asm volatile("msr fpsr, %0" ::"r"(t));
    }
#elif defined(__x86_64__)
    constexpr bool HAS_BANK = true;
    constexpr bool HAS_CTL = true;
    constexpr int N = 32; // 16 registers, two 64-bit halves each
    using fp_word = unsigned long long;

    __attribute__((always_inline)) inline void fp_bank_load(fp_word const* in)
    {
        __asm volatile(
            "movdqu 0(%[in]), %%xmm0\n\t"
            "movdqu 16(%[in]), %%xmm1\n\t"
            "movdqu 32(%[in]), %%xmm2\n\t"
            "movdqu 48(%[in]), %%xmm3\n\t"
            "movdqu 64(%[in]), %%xmm4\n\t"
            "movdqu 80(%[in]), %%xmm5\n\t"
            "movdqu 96(%[in]), %%xmm6\n\t"
            "movdqu 112(%[in]), %%xmm7\n\t"
            "movdqu 128(%[in]), %%xmm8\n\t"
            "movdqu 144(%[in]), %%xmm9\n\t"
            "movdqu 160(%[in]), %%xmm10\n\t"
            "movdqu 176(%[in]), %%xmm11\n\t"
            "movdqu 192(%[in]), %%xmm12\n\t"
            "movdqu 208(%[in]), %%xmm13\n\t"
            "movdqu 224(%[in]), %%xmm14\n\t"
            "movdqu 240(%[in]), %%xmm15\n\t"
            ::[in] "r"(in)
            : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",
              "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory");
    }
    void fp_bank_cycle(fp_word const* in, fp_word* out)
    {
        register uintptr_t rdi __asm("rdi") = KOS_SYS_SLEEP_NS;
        register uintptr_t rsi __asm("rsi") = BLOCK_NS;
        register uintptr_t rdx __asm("rdx") = 0;
        register uintptr_t r10 __asm("r10") = 0;
        register uintptr_t r8 __asm("r8") = 0;
        __asm volatile(
            "movdqu 0(%[in]), %%xmm0\n\t"
            "movdqu 16(%[in]), %%xmm1\n\t"
            "movdqu 32(%[in]), %%xmm2\n\t"
            "movdqu 48(%[in]), %%xmm3\n\t"
            "movdqu 64(%[in]), %%xmm4\n\t"
            "movdqu 80(%[in]), %%xmm5\n\t"
            "movdqu 96(%[in]), %%xmm6\n\t"
            "movdqu 112(%[in]), %%xmm7\n\t"
            "movdqu 128(%[in]), %%xmm8\n\t"
            "movdqu 144(%[in]), %%xmm9\n\t"
            "movdqu 160(%[in]), %%xmm10\n\t"
            "movdqu 176(%[in]), %%xmm11\n\t"
            "movdqu 192(%[in]), %%xmm12\n\t"
            "movdqu 208(%[in]), %%xmm13\n\t"
            "movdqu 224(%[in]), %%xmm14\n\t"
            "movdqu 240(%[in]), %%xmm15\n\t"
            "syscall\n\t"
            "movdqu %%xmm0, 0(%[out])\n\t"
            "movdqu %%xmm1, 16(%[out])\n\t"
            "movdqu %%xmm2, 32(%[out])\n\t"
            "movdqu %%xmm3, 48(%[out])\n\t"
            "movdqu %%xmm4, 64(%[out])\n\t"
            "movdqu %%xmm5, 80(%[out])\n\t"
            "movdqu %%xmm6, 96(%[out])\n\t"
            "movdqu %%xmm7, 112(%[out])\n\t"
            "movdqu %%xmm8, 128(%[out])\n\t"
            "movdqu %%xmm9, 144(%[out])\n\t"
            "movdqu %%xmm10, 160(%[out])\n\t"
            "movdqu %%xmm11, 176(%[out])\n\t"
            "movdqu %%xmm12, 192(%[out])\n\t"
            "movdqu %%xmm13, 208(%[out])\n\t"
            "movdqu %%xmm14, 224(%[out])\n\t"
            "movdqu %%xmm15, 240(%[out])\n\t"
            : "+r"(rdi), "+r"(rsi), "+r"(rdx), "+r"(r10), "+r"(r8)
            : [in] "r"(in), [out] "r"(out)
            : "rax", "rcx", "r9", "r11",
              "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",
              "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "cc", "memory");
    }

    // MXCSR's rounding control (bits 14:13) and its precision flag (bit 5).
    inline unsigned long long fp_ctl_read()
    {
        unsigned int m = 0;
        __asm volatile("stmxcsr %0" : "=m"(m));
        return m & ((3u << 13) | (1u << 5));
    }
    inline void fp_ctl_write(unsigned rmode, bool inexact)
    {
        unsigned int m = 0;
        __asm volatile("stmxcsr %0" : "=m"(m));
        m = (m & ~((3u << 13) | (1u << 5))) | ((rmode & 3u) << 13);
        if (inexact)
        {
            m = m | (1u << 5);
        }
        __asm volatile("ldmxcsr %0" ::"m"(m));
    }
#elif defined(__XTENSA__)
    constexpr bool HAS_BANK = false;
    constexpr bool HAS_CTL = true;
    constexpr int N = 16; // f0 .. f15, for the preemption arm
    using fp_word = unsigned long;

    inline void fp_bank_load(fp_word const*) {}
    void fp_bank_cycle(fp_word const*, fp_word* out)
    {
        kos::sleep_ns(BLOCK_NS);
        out[0] = 0;
    }

    // FCR's rounding mode (bits 1:0) and FSR's inexact flag (bit 7), newlib's fenv layout.
    inline unsigned long long fp_ctl_read()
    {
        unsigned long c = 0;
        unsigned long s = 0;
        __asm volatile("rur.fcr %0" : "=a"(c));
        __asm volatile("rur.fsr %0" : "=a"(s));
        return ((c & 3ul) << 8) | (s & (1ul << 7));
    }
    inline void fp_ctl_write(unsigned rmode, bool inexact)
    {
        unsigned long const c = rmode & 3u;
        unsigned long s = 0;
        if (inexact)
        {
            s = 1ul << 7;
        }
        __asm volatile("wur.fcr %0" ::"a"(c));
        __asm volatile("wur.fsr %0" ::"a"(s));
    }

    // The preemption arm: the f-registers are caller-saved, so only the level-1 interrupt frame
    // keeps them, and only across a preemption landing between the load and the snapshot.
    volatile uint32_t g_preempts = 0;

    void fp_preempt_cycle(fp_word const* in, fp_word* out, uint32_t spins)
    {
        __asm volatile(
            "lsi f0, %[in], 0\n\t"
            "lsi f1, %[in], 4\n\t"
            "lsi f2, %[in], 8\n\t"
            "lsi f3, %[in], 12\n\t"
            "lsi f4, %[in], 16\n\t"
            "lsi f5, %[in], 20\n\t"
            "lsi f6, %[in], 24\n\t"
            "lsi f7, %[in], 28\n\t"
            "lsi f8, %[in], 32\n\t"
            "lsi f9, %[in], 36\n\t"
            "lsi f10, %[in], 40\n\t"
            "lsi f11, %[in], 44\n\t"
            "lsi f12, %[in], 48\n\t"
            "lsi f13, %[in], 52\n\t"
            "lsi f14, %[in], 56\n\t"
            "lsi f15, %[in], 60\n\t"
            "1: addi %[n], %[n], -1\n\t"
            "bnez %[n], 1b\n\t"
            "ssi f0, %[out], 0\n\t"
            "ssi f1, %[out], 4\n\t"
            "ssi f2, %[out], 8\n\t"
            "ssi f3, %[out], 12\n\t"
            "ssi f4, %[out], 16\n\t"
            "ssi f5, %[out], 20\n\t"
            "ssi f6, %[out], 24\n\t"
            "ssi f7, %[out], 28\n\t"
            "ssi f8, %[out], 32\n\t"
            "ssi f9, %[out], 36\n\t"
            "ssi f10, %[out], 40\n\t"
            "ssi f11, %[out], 44\n\t"
            "ssi f12, %[out], 48\n\t"
            "ssi f13, %[out], 52\n\t"
            "ssi f14, %[out], 56\n\t"
            "ssi f15, %[out], 60\n\t"
            : [n] "+a"(spins)
            : [in] "a"(in), [out] "a"(out)
            : "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11",
              "f12", "f13", "f14", "f15", "memory");
    }

    // Above the checker: each wake preempts its spin.
    void preempter(void*)
    {
        fp_word junk[N];
        for (int i = 0; i < N; i++)
        {
            junk[i] = 0x7F000000u + static_cast<fp_word>(i);
        }
        while (true)
        {
            __asm volatile(
                "lsi f0, %[in], 0\n\t"
                "lsi f1, %[in], 4\n\t"
                "lsi f2, %[in], 8\n\t"
                "lsi f3, %[in], 12\n\t"
                "lsi f4, %[in], 16\n\t"
                "lsi f5, %[in], 20\n\t"
                "lsi f6, %[in], 24\n\t"
                "lsi f7, %[in], 28\n\t"
                "lsi f8, %[in], 32\n\t"
                "lsi f9, %[in], 36\n\t"
                "lsi f10, %[in], 40\n\t"
                "lsi f11, %[in], 44\n\t"
                "lsi f12, %[in], 48\n\t"
                "lsi f13, %[in], 52\n\t"
                "lsi f14, %[in], 56\n\t"
                "lsi f15, %[in], 60\n\t"
                ::[in] "a"(junk)
                : "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11",
              "f12", "f13", "f14", "f15", "memory");
            g_preempts = g_preempts + 1u;
            kos::sleep_ns(1000000u);
        }
    }
#else
    constexpr bool HAS_BANK = true;
    constexpr bool HAS_CTL = false;
    constexpr int N = 16; // s16 .. s31
    using fp_word = float;

    // s16-s31 are callee-saved, so the sleep call between the load and the snapshot keeps them
    // and only a switch can lose them. Always inlined: a real function's epilogue would restore
    // them and wipe out the load.
    __attribute__((always_inline)) inline void fp_bank_load(float const* in)
    {
        __asm volatile("vldmia %0, {s16-s31}" ::"r"(in)
                       : "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
                         "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31", "memory");
    }
    __attribute__((always_inline)) inline void fp_bank_cycle(float const* in, float* out)
    {
        fp_bank_load(in);
        kos::sleep_ns(BLOCK_NS);
        __asm volatile("vstmia %0, {s16-s31}" ::"r"(out) : "memory");
    }
    inline unsigned long long fp_ctl_read()
    {
        return 0;
    }
    inline void fp_ctl_write(unsigned, bool) {}
#endif

    // The trasher's control values differ from the checker's in both fields, or the seeded
    // values survive by accident and the control arm proves nothing.
    constexpr unsigned RMODE_TRASH = 1;
    constexpr unsigned RMODE_SEED = 2;

    // Lower priority than the checker, so it runs whenever the checker is blocked.
    void trasher(void*)
    {
        fp_word junk[N];
        for (int i = 0; i < N; i++)
        {
            junk[i] = static_cast<fp_word>(0x9000000000000000ull) + static_cast<fp_word>(i);
        }
        while (true)
        {
            fp_bank_load(junk);
            fp_ctl_write(RMODE_TRASH, false);
            for (volatile int i = 0; i < 2000;)
            {
                i = i + 1;
            }
        }
    }

    // The first index where the two banks differ, or -1.
    int first_bad(fp_word const* rd, fp_word const* sent)
    {
        for (int i = 0; i < N; i++)
        {
            if (rd[i] != sent[i])
            {
                return i;
            }
        }
        return -1;
    }

    void checker(void*)
    {
        fp_word sent[N];
        for (int i = 0; i < N; i++)
        {
            sent[i] = static_cast<fp_word>(0x0123456789ab0000ull) + static_cast<fp_word>(i);
        }
        int rounds = 0;
        while (true)
        {
            fp_ctl_write(RMODE_SEED, true);
            unsigned long long const ctl_sent = fp_ctl_read();
            fp_word rd[N];
            fp_bank_cycle(sent, rd);
            unsigned long long const ctl_read = fp_ctl_read();

            int bad = -1;
            if (HAS_BANK)
            {
                bad = first_bad(rd, sent);
            }
            char b[96];
            if (bad >= 0)
            {
#if defined(__aarch64__) or defined(__x86_64__)
                ksnprintf(b, sizeof(b), "  FP FAIL: register %d half %d (round %d)\n",
                          bad / 2, bad % 2, rounds);
#else
                ksnprintf(b, sizeof(b), "  FP FAIL: s%d = %d, expected %d (round %d)\n",
                          16 + bad, static_cast<int>(rd[bad]), static_cast<int>(sent[bad]),
                          rounds);
#endif
                kos::print(b);
                continue;
            }
            if (HAS_CTL and ctl_read != ctl_sent)
            {
                ksnprintf(b, sizeof(b), "  FP FAIL: control 0x%x, expected 0x%x (round %d)\n",
                          static_cast<unsigned>(ctl_read), static_cast<unsigned>(ctl_sent),
                          rounds);
                kos::print(b);
                continue;
            }
#if defined(__XTENSA__)
            uint32_t const before = g_preempts;
            fp_preempt_cycle(sent, rd, 4000000u);
            if (g_preempts == before)
            {
                ksnprintf(b, sizeof(b), "  FP FAIL: no preemption landed in the spin (round %d)\n",
                          rounds);
                kos::print(b);
                continue;
            }
            bad = first_bad(rd, sent);
            if (bad >= 0)
            {
                ksnprintf(b, sizeof(b), "  FP FAIL: f%d across a preemption (round %d)\n", bad,
                          rounds);
                kos::print(b);
                continue;
            }
#endif
            rounds++;
            if ((rounds % 10) == 0)
            {
                ksnprintf(b, sizeof(b), "  FP OK: %d rounds, FP state preserved across switch\n",
                          rounds);
                kos::print(b);
            }
        }
    }
}

int main(int, char**)
{
    kos::print("FP switch test: checker verifies its FP state survives a switch while\n");
    kos::print("a lower-priority trasher clobbers it. Result lines follow.\n\n");

    // All on ONE core: on another core the trasher clobbers registers the checker never sees,
    // and a dropped restore passes.
    uint32_t core_mask = 0;
#if KICKOS_KERNEL_CORES > 1
    int const cores = kos_task_cores(KOS_TASK_NONE);
    if (cores <= 0)
    {
        kos::print("  FP FAIL: this task has no core to pin the pair to\n");
        return 1;
    }
    core_mask = static_cast<uint32_t>(cores) & (0u - static_cast<uint32_t>(cores));
#endif
    auto const c = kos::thread::create(checker, nullptr, "checker", 20, KOS_POLICY_FIFO, 0,
                                       nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0,
                                       nullptr, KOS_TASK_NONE, core_mask);
    auto const t = kos::thread::create(trasher, nullptr, "trasher", 5, KOS_POLICY_FIFO, 0,
                                       nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0,
                                       nullptr, KOS_TASK_NONE, core_mask);
    bool ok = c.valid() and t.valid();
#if defined(__XTENSA__)
    auto const p = kos::thread::create(preempter, nullptr, "preempter", 25, KOS_POLICY_FIFO, 0,
                                       nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0,
                                       nullptr, KOS_TASK_NONE, core_mask);
    ok = ok and p.valid();
#endif
    if (not ok)
    {
        kos::print("  FP FAIL: a test thread could not be created\n");
        return 1;
    }

    static kos::Semaphore idle(0);
    while (true)
    {
        idle.wait();
    }
}
