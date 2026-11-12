// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The FP state an FP-live caller gets back from the SVC slow path, on both of its arms. A register
// IPC call the fastpath refuses has had its {s0-s15, FPSCR} lazily saved by the fastpath's own FP
// push, so the slow path's return pops them from the frame it builds on the kernel block, which
// holds them only if the slow path copied them there. A plain syscall reaches the slow path with
// the lazy save still pending, so the return pops nothing and the registers stay live. On both,
// a Secure ARMv8-M return that drops CONTROL_S.SFPA costs the caller its FPSCR at its next FP
// instruction. The kernel touches no FP register in between, so every one must come back as set.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>
#include <kickos/sys/abi.h>

#include <stddef.h>

#if !defined(__arm__) || !defined(__ARM_FP)
#error "svcfp sets the ARM FP register file; it must not be built soft-float or for another ISA"
#endif

extern "C" int32_t arch_syscall_reg(uint32_t* io);
extern "C" uintptr_t arch_syscall(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                  uintptr_t a3);

namespace
{
    // A rounding mode, flush-to-zero, default NaN, two flags and every cumulative exception bit,
    // none of which the reset value or a C runtime leaves set.
    constexpr uint32_t FPSCR_MARK = 0x63C0009Fu;
    constexpr uint32_t FPSCR_RMODE_RZ = 0x00C00000u;

    // The asm below reads and writes it at these literal offsets.
    struct Run
    {
        uint32_t in[16];
        uint32_t out[16];
        uint32_t io[KOS_CALL_REG_WORDS + 3];
        uint32_t mark;
        uint32_t was;
        uint32_t set;
        uint32_t got;
        int32_t rc;
        uint32_t nr;
    };
    static_assert(offsetof(Run, out) == 64 and offsetof(Run, io) == 128
                      and offsetof(Run, mark) == 160 and offsetof(Run, was) == 164
                      and offsetof(Run, set) == 168 and offsetof(Run, got) == 172
                      and offsetof(Run, rc) == 176 and offsetof(Run, nr) == 180,
                  "the asm's offsets no longer match Run");

// One block from the first FP write to the last read: the compiler owns s0-s15 everywhere else,
// and FPSCR is put back before it does.
#define SVCFP_ENTER                                                                                \
    "vmrs   r1, fpscr          \n\t"                                                              \
    "str    r1, [%[r], #164]   \n\t"                                                              \
    "vldmia %[r], {s0-s15}     \n\t"                                                              \
    "ldr    r1, [%[r], #160]   \n\t"                                                              \
    "vmsr   fpscr, r1          \n\t"                                                              \
    "vmrs   r1, fpscr          \n\t"                                                              \
    "str    r1, [%[r], #168]   \n\t"
#define SVCFP_LEAVE                                                                                \
    "str    r0, [%[r], #176]   \n\t"                                                              \
    "vmrs   r1, fpscr          \n\t"                                                              \
    "str    r1, [%[r], #172]   \n\t"                                                              \
    "add    r1, %[r], #64      \n\t"                                                              \
    "vstmia r1, {s0-s15}       \n\t"                                                              \
    "ldr    r1, [%[r], #164]   \n\t"                                                              \
    "vmsr   fpscr, r1          \n\t"
#define SVCFP_CLOBBERS                                                                             \
    "r0", "r1", "r2", "r3", "r12", "lr", "cc", "memory", "s0", "s1", "s2", "s3", "s4", "s5", "s6", \
        "s7", "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15"

    // The register call: KOS_CAP_NONE, refused by the leaf after the fastpath's FP push.
    void run_refused_call(Run* r)
    {
        __asm volatile(SVCFP_ENTER "add    r0, %[r], #128     \n\t"
                                   "bl     arch_syscall_reg   \n\t" SVCFP_LEAVE
                       :
                       : [r] "r"(r)
                       : SVCFP_CLOBBERS);
    }

    // A plain syscall, with no FP instruction in the handler before its return.
    void run_plain_call(Run* r)
    {
        __asm volatile(SVCFP_ENTER "ldr    r0, [%[r], #180]   \n\t"
                                   "bl     arch_syscall       \n\t" SVCFP_LEAVE
                       :
                       : [r] "r"(r)
                       : SVCFP_CLOBBERS);
    }

    void seed(Run* r)
    {
        *r = {};
        for (uint32_t i = 0; i < 16u; i++)
        {
            r->in[i] = 0x5EED0000u | (i << 8) | (0xF0u - i);
        }
        r->io[0] = KOS_SYS_CALL_REG;
        r->io[1] = KOS_CAP_NONE;
        r->mark = FPSCR_MARK;
        r->nr = KOS_SYS_CLOCK_NOW;
    }

    // False after printing what did not come back.
    bool intact(Run const* r, char const* arm)
    {
        char msg[128];
        if ((r->set & FPSCR_RMODE_RZ) != FPSCR_RMODE_RZ)
        {
            ksnprintf(msg, sizeof(msg),
                      "[svcfp] ERROR: %s: the FPSCR mark did not take, so nothing here was "
                      "witnessed\n",
                      arm);
            kos::print(msg);
            return false;
        }
        bool ok = true;
        if (r->got != r->set)
        {
            ksnprintf(msg, sizeof(msg), "[svcfp] CORRUPTED: %s: FPSCR 0x%x came back 0x%x\n", arm,
                      static_cast<unsigned>(r->set), static_cast<unsigned>(r->got));
            kos::print(msg);
            ok = false;
        }
        for (uint32_t i = 0; i < 16u; i++)
        {
            if (r->out[i] != r->in[i])
            {
                ksnprintf(msg, sizeof(msg), "[svcfp] CORRUPTED: %s: s%u 0x%x came back 0x%x\n",
                          arm, static_cast<unsigned>(i), static_cast<unsigned>(r->in[i]),
                          static_cast<unsigned>(r->out[i]));
                kos::print(msg);
                ok = false;
            }
        }
        return ok;
    }

    Run g_run;
}

int main(int, char**)
{
    char msg[128];
    seed(&g_run);
    run_refused_call(&g_run);
    if (g_run.rc != KOS_CALL_REG_FALLBACK)
    {
        ksnprintf(msg, sizeof(msg),
                  "[svcfp] ERROR: the refused call answered %d, not the dispatch\n",
                  static_cast<int>(g_run.rc));
        kos::print(msg);
        return 1;
    }
    bool const copied = intact(&g_run, "refused register call");
    seed(&g_run);
    run_plain_call(&g_run);
    bool const live = intact(&g_run, "plain syscall");
    if (not copied or not live)
    {
        return 1;
    }
    kos::print("[svcfp] PASS: s0-s15 and FPSCR survived a refused register call and a plain "
               "syscall\n");
    return 0;
}
