// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The x86_64 vector-state arms: x87, SSE and AVX state across both switch paths, across cores,
// at a new thread's first instruction, and an unmasked floating-point fault at ring 3.
//
// Every instruction that touches that state is in the assembly below, never in compiled code:
// a pattern held across a syscall or a preemption must not meet a register the compiler chose.

#include "selftest.h"

#include <stddef.h>

#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST) && KICKOS_FAULT_ISOLATION         \
    && defined(__x86_64__) && not KICKOS_ARCH_SIM

namespace selftest
{
    // What a pattern loads into the vector state and what the arm reads back out of it, at the
    // offsets the assembly below spells.
    struct VecImage
    {
        uint8_t ymm[16][32];
        int32_t x87[8]; // st(0) first
        uint32_t mxcsr;
        uint16_t fcw;
        uint16_t pad;
    };
    static_assert(offsetof(VecImage, x87) == 512, "the assembly loads the x87 stack at 512");
    static_assert(offsetof(VecImage, mxcsr) == 544, "the assembly loads MXCSR at 544");
    static_assert(offsetof(VecImage, fcw) == 548, "the assembly loads the x87 control at 548");

    // A new thread's state as its entry finds it: the YMM registers, MXCSR, and the x87
    // environment FNSTENV writes, whose control, status and tag words lead 4-byte fields.
    struct VecClean
    {
        uint8_t ymm[16][32];
        uint32_t mxcsr;
        uint8_t env[28];
    };
    static_assert(offsetof(VecClean, mxcsr) == 512, "the clean entry stores MXCSR at 512");
    static_assert(offsetof(VecClean, env) == 516, "the clean entry stores the x87 env at 516");
    static_assert(sizeof(VecClean) <= 584, "the clean entry reserves 584 bytes of its stack");
}

extern "C"
{
    // Loads `load`, makes the syscall (nr, a0, a1) while holding it, and stores the state into
    // `out`. Returns the syscall's result.
    KICKOS_SELFTEST_LOCAL int selftest_vec_hold(selftest::VecImage const* load,
                                                    selftest::VecImage* out, uintptr_t nr,
                                                    uintptr_t a0, uintptr_t a1);
    // Loads `load` and spins, counting in *own, until *other moves or `bound` polls pass, then
    // stores the state into `out`. Returns 1 when *other moved.
    KICKOS_SELFTEST_LOCAL int selftest_vec_spin(selftest::VecImage const* load,
                                                    selftest::VecImage* out,
                                                    uint32_t const volatile* other,
                                                    uint64_t bound, uint32_t volatile* own);
    // A thread entry: stores the state its first instruction finds and hands it, with the
    // thread's argument, to selftest_vec_clean_report.
    KICKOS_SELFTEST_LOCAL void selftest_vec_clean_entry(void* arg);
    KICKOS_SELFTEST_LOCAL void selftest_vec_clean_report(selftest::VecClean const* seen,
                                                             void* arg);
    // Unmask divide-by-zero and divide by zero: SSE, which raises #XM, and x87, which leaves
    // the exception pending to the FWAIT that raises #MF. Where nothing is raised they return
    // MXCSR, or the x87 status word, as the divide left it.
    KICKOS_SELFTEST_LOCAL uint32_t selftest_vec_fault_xm(void);
    KICKOS_SELFTEST_LOCAL uint32_t selftest_vec_fault_mf(void);
}

// clang-format off
#define KOS_VEC_LOAD(r)                                                                       \
    "    vmovdqu   0(" r "), %ymm0\n    vmovdqu  32(" r "), %ymm1\n"                         \
    "    vmovdqu  64(" r "), %ymm2\n    vmovdqu  96(" r "), %ymm3\n"                         \
    "    vmovdqu 128(" r "), %ymm4\n    vmovdqu 160(" r "), %ymm5\n"                         \
    "    vmovdqu 192(" r "), %ymm6\n    vmovdqu 224(" r "), %ymm7\n"                         \
    "    vmovdqu 256(" r "), %ymm8\n    vmovdqu 288(" r "), %ymm9\n"                         \
    "    vmovdqu 320(" r "), %ymm10\n   vmovdqu 352(" r "), %ymm11\n"                        \
    "    vmovdqu 384(" r "), %ymm12\n   vmovdqu 416(" r "), %ymm13\n"                        \
    "    vmovdqu 448(" r "), %ymm14\n   vmovdqu 480(" r "), %ymm15\n"                        \
    "    fildl 540(" r ")\n    fildl 536(" r ")\n    fildl 532(" r ")\n    fildl 528(" r ")\n" \
    "    fildl 524(" r ")\n    fildl 520(" r ")\n    fildl 516(" r ")\n    fildl 512(" r ")\n" \
    "    fldcw 548(" r ")\n    ldmxcsr 544(" r ")\n"
#define KOS_VEC_STORE_YMM(r)                                                                  \
    "    vmovdqu %ymm0,   0(" r ")\n    vmovdqu %ymm1,  32(" r ")\n"                         \
    "    vmovdqu %ymm2,  64(" r ")\n    vmovdqu %ymm3,  96(" r ")\n"                         \
    "    vmovdqu %ymm4, 128(" r ")\n    vmovdqu %ymm5, 160(" r ")\n"                         \
    "    vmovdqu %ymm6, 192(" r ")\n    vmovdqu %ymm7, 224(" r ")\n"                         \
    "    vmovdqu %ymm8, 256(" r ")\n    vmovdqu %ymm9, 288(" r ")\n"                         \
    "    vmovdqu %ymm10, 320(" r ")\n   vmovdqu %ymm11, 352(" r ")\n"                        \
    "    vmovdqu %ymm12, 384(" r ")\n   vmovdqu %ymm13, 416(" r ")\n"                        \
    "    vmovdqu %ymm14, 448(" r ")\n   vmovdqu %ymm15, 480(" r ")\n"
// The control words go back to the psABI's before the return, which keeps them for its caller,
// and the x87 stack is left empty, as a call boundary requires.
#define KOS_VEC_STORE(r)                                                                      \
    KOS_VEC_STORE_YMM(r)                                                                      \
    "    fnstcw 548(" r ")\n    stmxcsr 544(" r ")\n"                                         \
    "    fistpl 512(" r ")\n    fistpl 516(" r ")\n    fistpl 520(" r ")\n    fistpl 524(" r ")\n" \
    "    fistpl 528(" r ")\n    fistpl 532(" r ")\n    fistpl 536(" r ")\n    fistpl 540(" r ")\n" \
    "    pushq $0x1f80\n    ldmxcsr (%rsp)\n    movw $0x37f, (%rsp)\n    fldcw (%rsp)\n"      \
    "    addq $8, %rsp\n    vzeroupper\n"
#define KOS_VEC_FN(name)                                                                      \
    "    .balign 16\n    .globl " name "\n    .hidden " name "\n    .type " name ", @function\n" \
    name ":\n"

__asm__(".pushsection .text, \"ax\", @progbits\n"
        KOS_VEC_FN("selftest_vec_hold")
        "    pushq %rbx\n    pushq %r12\n    movq %rdi, %rbx\n    movq %rsi, %r12\n"
        KOS_VEC_LOAD("%rbx")
        "    movq %rdx, %rdi\n    movq %rcx, %rsi\n    movq %r8, %rdx\n"
        "    xorl %r10d, %r10d\n    xorl %r8d, %r8d\n    syscall\n"
        KOS_VEC_STORE("%r12")
        "    popq %r12\n    popq %rbx\n    ret\n"

        KOS_VEC_FN("selftest_vec_spin")
        "    pushq %rbx\n    pushq %r12\n    movq %rdi, %rbx\n    movq %rsi, %r12\n"
        KOS_VEC_LOAD("%rbx")
        "    movl (%rdx), %r9d\n"
        "1:  incl (%r8)\n    cmpl (%rdx), %r9d\n    jne 2f\n    decq %rcx\n    jnz 1b\n"
        "    xorl %eax, %eax\n    jmp 3f\n"
        "2:  movl $1, %eax\n"
        "3:\n"
        KOS_VEC_STORE("%r12")
        "    popq %r12\n    popq %rbx\n    ret\n"

        // The entry sees rsp 8 modulo 16, so the 584-byte record leaves the call aligned.
        KOS_VEC_FN("selftest_vec_clean_entry")
        "    movq %rdi, %rsi\n    subq $584, %rsp\n"
        KOS_VEC_STORE_YMM("%rsp")
        "    stmxcsr 512(%rsp)\n    fnstenv 516(%rsp)\n"
        "    movq %rsp, %rdi\n    call selftest_vec_clean_report\n"
        "    addq $584, %rsp\n    ret\n"

        // MXCSR 0x1F80 with ZM clear, then 1.0f / 0.0f.
        KOS_VEC_FN("selftest_vec_fault_xm")
        "    pushq $0x1d80\n    ldmxcsr (%rsp)\n"
        "    movl $0x3f800000, %eax\n    vmovd %eax, %xmm0\n    vxorps %xmm1, %xmm1, %xmm1\n"
        "    vdivss %xmm1, %xmm0, %xmm0\n"
        "    stmxcsr (%rsp)\n    movl (%rsp), %eax\n    movl $0x1f80, (%rsp)\n"
        "    ldmxcsr (%rsp)\n    addq $8, %rsp\n    vzeroupper\n    ret\n"

        // The control word 0x37F with ZM clear, then 1 / 0.
        KOS_VEC_FN("selftest_vec_fault_mf")
        "    pushq $0x37b\n    fldcw (%rsp)\n    fldz\n    fld1\n    fdiv %st(1), %st\n"
        "    fwait\n    fnstsw %ax\n    fninit\n    addq $8, %rsp\n    ret\n"

        ".popsection\n");
// clang-format on

namespace selftest
{
    constexpr uint32_t VEC_MXCSR_INIT = 0x1F80u;
    constexpr uint16_t VEC_FCW_INIT = 0x037Fu;
    // Every x87 register tagged empty.
    constexpr uint16_t VEC_FTW_EMPTY = 0xFFFFu;
    // Polls of the preemption spin: seconds of emulated time, far past any slice.
    constexpr uint64_t VEC_SPIN_BOUND = 1ull << 28;
    constexpr uint32_t VEC_SPIN_JOIN_US = 4u * STALL_TOLERANT_US;

    // A pattern of its own per seed in every byte of the sixteen YMM registers and in the x87
    // stack, the rounding field of MXCSR set to `rc` and the precision field of the x87 control
    // word to `pc`. The x87 values fit 24 bits, so every precision holds them exactly.
    void vec_pattern(VecImage* p, uint32_t seed, uint32_t rc, uint32_t pc)
    {
        for (unsigned r = 0; r < 16; r++)
        {
            for (unsigned b = 0; b < 32; b++)
            {
                p->ymm[r][b] = static_cast<uint8_t>(seed * 37u + r * 32u + b + 1u);
            }
        }
        for (unsigned i = 0; i < 8; i++)
        {
            p->x87[i] = static_cast<int32_t>(seed * 1000u + i + 1u);
        }
        p->mxcsr = VEC_MXCSR_INIT | (rc << 13);
        p->fcw = static_cast<uint16_t>((VEC_FCW_INIT & ~0x300u) | (pc << 8));
        p->pad = 0;
    }

    bool vec_same(VecImage const& a, VecImage const& b)
    {
        return memcmp(a.ymm, b.ymm, sizeof(a.ymm)) == 0
               and memcmp(a.x87, b.x87, sizeof(a.x87)) == 0 and a.mxcsr == b.mxcsr
               and a.fcw == b.fcw;
    }

    // --- one core, the voluntary switch -------------------------------------------------
    // The holder parks on a semaphore with its pattern loaded. The poster, on the same core and
    // below it, loads its own and posts, which switches it out in the syscall for the holder:
    // the holder is restored over the poster's registers, and the poster after the holder is
    // gone. Priority on that core alone parks the holder before the poster runs.
    VecImage g_vb_load[2];
    VecImage g_vb_seen[2];
    int32_t g_vb_rc[2];
    Atomic<uint32_t, Order::RELAXED> g_vb_holder_done{0};
    uint32_t g_vb_poster_after = 0;

    void vb_holder(void*) // caps: S(WAIT)@1
    {
        g_vb_rc[0] = selftest_vec_hold(&g_vb_load[0], &g_vb_seen[0], KOS_SYS_SEM_WAIT,
                                           KOS_SPAWN_DELEGATED_CAP0, KOS_TIMEOUT_NONE);
        g_vb_holder_done = 1;
    }
    void vb_poster(void*) // caps: S(SIGNAL)@1
    {
        g_vb_rc[1] = selftest_vec_hold(&g_vb_load[1], &g_vb_seen[1], KOS_SYS_SEM_POST,
                                           KOS_SPAWN_DELEGATED_CAP0, 0);
        g_vb_poster_after = g_vb_holder_done;
    }

    void t_vector_survives_block()
    {
        TAP_ASK(.workers = 2, .sems = 1);
        vec_pattern(&g_vb_load[0], 1, 1, 0);
        vec_pattern(&g_vb_load[1], 2, 2, 2);
        memset(g_vb_seen, 0, sizeof(g_vb_seen));
        g_vb_rc[0] = -1;
        g_vb_rc[1] = -1;
        g_vb_holder_done = 0;
        g_vb_poster_after = 0;
        kos_cap_t s = KOS_CAP_NONE;
        kos::thread::Handle h;
        kos::thread::Handle p;
        ArmHold hold;
        TAP_HOLD(hold.cap(&s) and hold.thread(&h) and hold.thread(&p));
        TAP_CHECK(kos_sem_create(0, &s) == 0);
        kos_cap_grant const hcaps[] = {{s, KOS_CAP_WAIT}};
        kos_cap_grant const pcaps[] = {{s, KOS_CAP_SIGNAL}};
        h = kos::thread::create_caps(vb_holder, nullptr, "vbhold", TAP_PRIO_PARKS + 1, hcaps, 1,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                     KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(h.valid());
        p = kos::thread::create_caps(vb_poster, nullptr, "vbpost", TAP_PRIO_PARKS, pcaps, 1,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                     KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(p.valid());
        TAP_CHECK(hold.joined());
        tap::diag("holder rc %d mxcsr 0x%x fcw 0x%x, poster rc %d mxcsr 0x%x fcw 0x%x, "
                  "holder ran inside the poster's post: %u",
                  static_cast<int>(g_vb_rc[0]), static_cast<unsigned>(g_vb_seen[0].mxcsr),
                  static_cast<unsigned>(g_vb_seen[0].fcw), static_cast<int>(g_vb_rc[1]),
                  static_cast<unsigned>(g_vb_seen[1].mxcsr),
                  static_cast<unsigned>(g_vb_seen[1].fcw),
                  static_cast<unsigned>(g_vb_poster_after));
        TAP_CHECK(g_vb_rc[0] == 0 and g_vb_rc[1] == 0);
        // The poster was switched out for the holder, so both crossed a voluntary switch.
        TAP_CHECK(g_vb_poster_after == 1);
        TAP_CHECK(vec_same(g_vb_load[0], g_vb_seen[0]));
        TAP_CHECK(vec_same(g_vb_load[1], g_vb_seen[1]));
        TAP_CHECK(kos_sem_destroy(s) == 0);
        s = KOS_CAP_NONE;
    }

    // --- one core, the interrupt-exit switch ----------------------------------------------
    // Two round-robin spinners on one core, each holding its pattern and counting until the
    // other's count moves. Neither blocks, so the other's count moves only once a slice has
    // ended and the timer's interrupt switched this one out.
    VecImage g_vp_load[2];
    VecImage g_vp_seen[2];
    int32_t g_vp_moved[2];
    uint32_t volatile g_vp_count[2];

    void vp_spinner(void* arg)
    {
        uintptr_t const me = reinterpret_cast<uintptr_t>(arg);
        g_vp_moved[me] = selftest_vec_spin(&g_vp_load[me], &g_vp_seen[me],
                                               &g_vp_count[1u - me], VEC_SPIN_BOUND,
                                               &g_vp_count[me]);
        g_vp_count[me] = g_vp_count[me] + 1u;
    }

    uint64_t vec_quantum_ns()
    {
        // An emulated clock's granule is coarse, so the slice is held above it.
        uint64_t e0 = kos_clock_now();
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
        uint64_t quantum = 1000000ull;
        if (quantum < (e2 - e1) * 4)
        {
            quantum = (e2 - e1) * 4;
        }
        return quantum;
    }

    void t_vector_survives_preempt()
    {
        TAP_ASK(.workers = 2);
        vec_pattern(&g_vp_load[0], 3, 3, 3);
        vec_pattern(&g_vp_load[1], 4, 1, 2);
        memset(g_vp_seen, 0, sizeof(g_vp_seen));
        g_vp_moved[0] = -1;
        g_vp_moved[1] = -1;
        g_vp_count[0] = 0;
        g_vp_count[1] = 0;
        uint32_t const quantum = static_cast<uint32_t>(vec_quantum_ns());
        kos::thread::Handle w[2];
        ArmHold hold(VEC_SPIN_JOIN_US);
        TAP_HOLD(hold.thread(&w[0]) and hold.thread(&w[1]));
        for (uintptr_t i = 0; i < 2; i++)
        {
            w[i] = kos::thread::create_caps(vp_spinner, reinterpret_cast<void*>(i), "vpspin",
                                            TAP_PRIO_PARKS, nullptr, 0, KOS_POLICY_RR, quantum,
                                            false, nullptr, 0, 0, nullptr, KOS_TASK_NONE,
                                            nullptr, 0, TAP_PIN_CORE);
            TAP_CHECK(w[i].valid());
        }
        TAP_CHECK(hold.joined());
        tap::diag("spinners saw the other count: %d %d; mxcsr 0x%x 0x%x, fcw 0x%x 0x%x",
                  static_cast<int>(g_vp_moved[0]), static_cast<int>(g_vp_moved[1]),
                  static_cast<unsigned>(g_vp_seen[0].mxcsr),
                  static_cast<unsigned>(g_vp_seen[1].mxcsr),
                  static_cast<unsigned>(g_vp_seen[0].fcw), static_cast<unsigned>(g_vp_seen[1].fcw));
        // One core, neither blocking: the other's count moved only because this one was
        // preempted.
        TAP_CHECK(g_vp_moved[0] == 1 and g_vp_moved[1] == 1);
        TAP_CHECK(vec_same(g_vp_load[0], g_vp_seen[0]));
        TAP_CHECK(vec_same(g_vp_load[1], g_vp_seen[1]));
    }

    // --- a new thread starts clean -------------------------------------------------------
    struct VecVerdict
    {
        uint32_t bad;
        uint32_t mxcsr;
        uint16_t fcw;
        uint16_t fsw;
        uint16_t ftw;
        uint16_t pad;
    };
    VecVerdict g_vc_seen;

    VecVerdict vec_judge(VecClean const* seen)
    {
        VecVerdict v{};
        for (unsigned r = 0; r < 16; r++)
        {
            for (unsigned b = 0; b < 32; b++)
            {
                if (seen->ymm[r][b] != 0)
                {
                    v.bad |= 1u << 0;
                }
            }
        }
        v.mxcsr = seen->mxcsr;
        memcpy(&v.fcw, &seen->env[0], sizeof(v.fcw));
        memcpy(&v.fsw, &seen->env[4], sizeof(v.fsw));
        memcpy(&v.ftw, &seen->env[8], sizeof(v.ftw));
        if (v.mxcsr != VEC_MXCSR_INIT)
        {
            v.bad |= 1u << 1;
        }
        if (v.fcw != VEC_FCW_INIT)
        {
            v.bad |= 1u << 2;
        }
        if (v.fsw != 0)
        {
            v.bad |= 1u << 3;
        }
        if (v.ftw != VEC_FTW_EMPTY)
        {
            v.bad |= 1u << 4;
        }
        return v;
    }

    void vec_patterned(void*)
    {
        VecImage load;
        VecImage seen;
        vec_pattern(&load, 5, 3, 0);
        (void)selftest_vec_hold(&load, &seen, KOS_SYS_CLOCK_NOW, 0, 0);
    }

    bool vec_clean_ok(VecVerdict const& v, char const* who)
    {
        tap::diag("%s: bad 0x%x, mxcsr 0x%x, fcw 0x%x, fsw 0x%x, ftw 0x%x", who,
                  static_cast<unsigned>(v.bad), static_cast<unsigned>(v.mxcsr),
                  static_cast<unsigned>(v.fcw), static_cast<unsigned>(v.fsw),
                  static_cast<unsigned>(v.ftw));
        return v.bad == 0;
    }

    // One thread, run to its end in a hold of its own: the next thread may take its slot.
    bool vec_run(kos::thread::Handle* t, void (*entry)(void*), void* arg, char const* name)
    {
        ArmHold hold;
        if (not hold.thread(t))
        {
            return false;
        }
        *t = kos::thread::create_caps(entry, arg, name, TAP_PRIO_PARKS, nullptr, 0);
        return t->valid() and hold.joined();
    }

    void t_vector_starts_clean()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .endpoints = 1);
        // A thread in the slot a patterned thread vacated: the pool hands the freed slot back.
        kos::thread::Handle prev;
        TAP_CHECK(vec_run(&prev, vec_patterned, nullptr, "vcpat"));
        memset(&g_vc_seen, 0xA5, sizeof(g_vc_seen));
        kos::thread::Handle next;
        TAP_CHECK(vec_run(&next, selftest_vec_clean_entry, &g_vc_seen, "vcnew"));
        tap::diag("patterned slot %u, new thread slot %u",
                  static_cast<unsigned>(prev.id() & 0xFFFFu),
                  static_cast<unsigned>(next.id() & 0xFFFFu));
        TAP_CHECK((prev.id() & 0xFFFFu) == (next.id() & 0xFFFFu));
        TAP_CHECK(vec_clean_ok(g_vc_seen, "same task, reused slot"));

        // A thread of another task, whose verdict comes back over an endpoint.
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t task = KOS_TASK_NONE;
        kos::thread::Handle other;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&task) and hold.thread(&other));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        kos_cap_grant const caps[] = {{ep, KOS_CAP_SIGNAL}};
        other = kos::thread::create(selftest_vec_clean_entry, nullptr, "vctask", TAP_PRIO_PARKS,
                                    KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0, nullptr, 0,
                                    caps, 1, 0, nullptr, task);
        TAP_CHECK(other.valid());
        VecVerdict v;
        memset(&v, 0xA5, sizeof(v));
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, &v, kos_call_lens_pack(0, sizeof(v)), &o);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(v)));
        TAP_CHECK(hold.joined());
        TAP_CHECK(vec_clean_ok(v, "another task"));
        TAP_CHECK(kos_task_kill(task) == 0);
        task = KOS_TASK_NONE;
        TAP_CHECK(kos_handle_close(ep) == 0);
        ep = KOS_CAP_NONE;
    }

    // --- an unmasked floating-point exception at ring 3 ------------------------------------
    // The divide-by-zero flag of MXCSR and of the x87 status word.
    constexpr uint32_t VEC_ZE = 1u << 2;

    // Takes main's call, so a death from here on answers it -KOS_EPIPE. KOS_CAP_NONE when no
    // call came.
    kos_cap_t vf_take_call()
    {
        uint32_t word = 0;
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, KOS_SPAWN_DELEGATED_CAP0, 0, KOS_TIMEOUT_NONE);
        if (kos_reply_recv(KOS_CAP_NONE, &word, kos_call_lens_pack(0, sizeof(word)), &opts) < 0)
        {
            return KOS_CAP_NONE;
        }
        return opts.info.reply_cap;
    }
    void vf_xm_victim(void*) // caps: E(WAIT)@1
    {
        kos_cap_t const call = vf_take_call();
        uint32_t const word = selftest_vec_fault_xm();
        // Reached only where the divide raised nothing.
        (void)kos_reply(call, &word, sizeof(word));
        kos_exit(1);
    }
    void vf_mf_victim(void*) // caps: E(WAIT)@1
    {
        kos_cap_t const call = vf_take_call();
        uint32_t const word = selftest_vec_fault_mf();
        (void)kos_reply(call, &word, sizeof(word));
        kos_exit(1);
    }
    void vf_sibling(void*) // caps: park@1
    {
        kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0, KOS_TIMEOUT_NONE);
        kos_exit(1);
    }

    // Each victim's task holds a sibling parked on a semaphore nobody posts, so only the task's
    // death releases it: a fault the kernel took for a kernel bug ends the image instead. The
    // victim divides holding main's call, so its death answers the call -KOS_EPIPE and a divide
    // that raised nothing answers it with the flags the divide set. *verdict: 1 when the task
    // died, 0 when the divide raised nothing and set the flag, -1 otherwise.
    void vec_fault_kills(void (*victim)(void*), char const* name, char const* what,
                         uint32_t* word, int* verdict)
    {
        *verdict = -1;
        *word = 0;
        kos_cap_t park = KOS_CAP_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t task = KOS_TASK_NONE;
        kos::thread::Handle sib;
        kos::thread::Handle vic;
        ArmHold hold;
        TAP_HOLD(hold.cap(&park) and hold.cap(&ep) and hold.task(&task) and hold.thread(&sib)
                 and hold.thread(&vic));
        TAP_CHECK(kos_sem_create(0, &park) == 0);
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        kos_cap_grant const pcaps[] = {{park, KOS_CAP_WAIT}};
        kos_cap_grant const vcaps[] = {{ep, KOS_CAP_WAIT}};
        sib = kos::thread::create(vf_sibling, nullptr, "vfsib", 10, KOS_POLICY_FIFO, 0, false,
                                  nullptr, 0, nullptr, 0, nullptr, 0, pcaps, 1, 0, nullptr, task);
        TAP_CHECK(sib.valid());
        vic = kos::thread::create(victim, nullptr, name, 10, KOS_POLICY_FIFO, 0, false, nullptr,
                                  0, nullptr, 0, nullptr, 0, vcaps, 1, 0, nullptr, task);
        TAP_CHECK(vic.valid());
        int32_t const n = kos_call_timed(ep, word, 0, sizeof(*word), STALL_TOLERANT_US);
        int verdict_now = -1;
        if (n == -KOS_EPIPE)
        {
            int const vj = vic.join(STALL_TOLERANT_US);
            int const sj = sib.join(STALL_TOLERANT_US);
            tap::diag("%s: call %d, victim join %d, parked sibling join %d", what,
                      static_cast<int>(n), vj, sj);
            if (vj == 0 and sj == 0)
            {
                verdict_now = 1;
            }
        }
        else
        {
            tap::diag("%s: call %d word 0x%x", what, static_cast<int>(n),
                      static_cast<unsigned>(*word));
            if (n == static_cast<int32_t>(sizeof(*word)) and (*word & VEC_ZE) != 0)
            {
                verdict_now = 0;
            }
        }
        int const killed = kos_task_kill(task);
        task = KOS_TASK_NONE;
        int const closed = kos_handle_close(ep);
        ep = KOS_CAP_NONE;
        int const freed = kos_handle_close(park);
        park = KOS_CAP_NONE;
        if (killed == 0 and closed == 0 and freed == 0)
        {
            *verdict = verdict_now;
        }
    }

    void t_vector_fault_contained()
    {
        TAP_ASK(.workers = 2, .tasks = 1, .sems = 1, .endpoints = 1);
        uint32_t word = 0;
        int mf = -1;
        vec_fault_kills(vf_mf_victim, "vfmf", "x87 divide by zero pending to FWAIT, #MF", &word,
                        &mf);
        TAP_CHECK(mf == 1);
        int xm = -1;
        vec_fault_kills(vf_xm_victim, "vfxm", "SSE divide by zero, #XM", &word, &xm);
        TAP_CHECK(xm >= 0);
        if (xm == 0)
        {
            // QEMU's TCG sets the flag of an unmasked SIMD exception and raises nothing.
            tap::partial("the divide set MXCSR.ZE (0x%x) with ZM clear and raised no #XM, so "
                         "this processor model leaves the SIMD fault unexercised",
                         static_cast<unsigned>(word));
        }
    }

#if KICKOS_KERNEL_CORES > 1
    // --- across cores ---------------------------------------------------------------------
    // The thread starts pinned to core 0, loads its pattern and re-pins itself to a core that is
    // not isolated in the syscall it holds the pattern across, so it is switched out on one core
    // and in on another.
    VecImage g_vm_load;
    VecImage g_vm_seen;
    int32_t g_vm_rc;
    uint32_t g_vm_away;
    uint32_t g_vm_core[2];

    void vm_mover(void*)
    {
        g_vm_core[0] = static_cast<uint32_t>(kos_core_current());
        g_vm_rc = selftest_vec_hold(&g_vm_load, &g_vm_seen, KOS_SYS_THREAD_SET_AFFINITY,
                                        kos_thread_self(), 1u << g_vm_away);
        g_vm_core[1] = static_cast<uint32_t>(kos_core_current());
    }

    void t_vector_survives_migrate()
    {
        uint32_t const iso = KOS_ISOLATED_CORES;
        g_vm_away = 0;
        for (uint32_t c = 1; c < static_cast<uint32_t>(KICKOS_KERNEL_CORES); c++)
        {
            if ((iso & (1u << c)) == 0)
            {
                g_vm_away = c;
                break;
            }
        }
        TAP_SKIP_UNLESS(g_vm_away != 0, "a move needs a non-isolated core beside the boot core");
        TAP_ASK(.workers = 1);
        vec_pattern(&g_vm_load, 6, 2, 0);
        memset(&g_vm_seen, 0, sizeof(g_vm_seen));
        g_vm_rc = -1;
        g_vm_core[0] = 0xFFu;
        g_vm_core[1] = 0xFFu;
        kos::thread::Handle t;
        ArmHold hold;
        TAP_HOLD(hold.thread(&t));
        t = kos::thread::create_caps(vm_mover, nullptr, "vmmove", TAP_PRIO_PARKS, nullptr, 0,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                     KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(t.valid());
        TAP_CHECK(hold.joined());
        tap::diag("re-pin rc %d, core %u before and %u after, mxcsr 0x%x fcw 0x%x",
                  static_cast<int>(g_vm_rc), static_cast<unsigned>(g_vm_core[0]),
                  static_cast<unsigned>(g_vm_core[1]), static_cast<unsigned>(g_vm_seen.mxcsr),
                  static_cast<unsigned>(g_vm_seen.fcw));
        TAP_CHECK(g_vm_rc == 0);
        TAP_CHECK(g_vm_core[0] == 0 and g_vm_core[1] == g_vm_away);
        TAP_CHECK(vec_same(g_vm_load, g_vm_seen));
    }
#endif
}

extern "C" void selftest_vec_clean_report(selftest::VecClean const* seen, void* arg)
{
    selftest::VecVerdict const v = selftest::vec_judge(seen);
    if (arg != nullptr)
    {
        *static_cast<selftest::VecVerdict*>(arg) = v;
        return;
    }
    (void)kos_send(KOS_SPAWN_DELEGATED_CAP0, &v, sizeof(v));
}

#endif
