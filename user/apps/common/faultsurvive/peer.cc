// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// faultsurvive's faulter, a program of its own so that its fault ends a task main is not in.
// KICKOS_FS_MODE is main.cc's; the one argument, where the mode takes an address, is that
// address in hex.

#include <kickos/apps/peer_run.h>
#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>
#include <kickos/sys/trap.h>

#include <stdint.h>

#include "fs.h"

// An unset mode compares equal to 0, so it would silently build that arm.
#ifndef KICKOS_FS_MODE
#error "KICKOS_FS_MODE must be set by this app's CMakeLists"
#endif

// rv32imac only: this app's CMakeLists puts that backend's include directory on modes 3, 4 and
// 12 alone.
#if (KICKOS_FS_MODE == 3 || KICKOS_FS_MODE == 4 || KICKOS_FS_MODE == 12) && defined(__riscv)
#include <kickos/arch/rv_trap_stack.h>
#endif

#if KICKOS_FS_MODE == 12
#if !defined(__riscv) || __riscv_xlen != 32
#error "KICKOS_FS_MODE 12 plants rv32imac's frame layout; no other backend defines it"
#endif

extern "C"
{
    // Reached only by a child that got past the probe, which U-mode cannot execute.
    void fs_forge_escalated(void*)
    {
        kos::print("[fs] [forge] ESCALATED: the child runs in M-mode\n");
    }

    void fs_forge_child(void*);
}

// The probe is the child's FIRST instruction, so a U-mode child dies at its entry.
__asm__(".text\n"
        ".balign 4\n"
        ".global fs_forge_child\n"
        ".type fs_forge_child, @function\n"
        "fs_forge_child:\n"
        "    csrr t0, mstatus\n"
        "    j fs_forge_escalated\n"
        ".size fs_forge_child, . - fs_forge_child\n");
#endif

// A synchronous fault the running instruction owns. The sim reaches the rule through SIGILL. An
// SP move shares one asm statement with KOS_TRAP_ILLEGAL_INSN, so nothing runs on the moved SP.
namespace
{
#if KICKOS_FS_MODE == 3
#if !defined(__riscv) && !defined(__RX__)
#error "KICKOS_FS_MODE 3 targets the software trap prologue (rv32imac/rxv3) only"
#endif
#endif
#if KICKOS_FS_MODE == 4
#if !defined(__riscv)
#error "KICKOS_FS_MODE 4 pins rv32imac's trap-frame figures; no other backend defines them"
#endif
    // EXACTLY the frame, so an entry that built the frame at the parked SP lands it flush with
    // stack_lo and runs every byte of its C dispatch below stack_lo, through the libc state and
    // into the band.
    constexpr uintptr_t FS_LOW_ROOM = KICKOS_RV_TRAP_FRAME;
    static_assert(FS_LOW_ROOM < KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_KERNEL_DEPTH,
                  "FS_LOW_ROOM leaves room for the frame and the kernel descent, so even an "
                  "entry that adopted this SP would stay above stack_lo and an intact band "
                  "would prove nothing");
    static_assert(KICKOS_RV_TRAP_KERNEL_DEPTH > KICKOS_REENT_CARVE,
                  "the kernel descent ends inside the libc state below stack_lo, so an intact "
                  "band would prove nothing");

#endif
#if KICKOS_FS_MODE == 5
#if !defined(__riscv) && !defined(__RX__)
#error "KICKOS_FS_MODE 5 needs this ISA's spelling for moving SP; it must not be built here"
#endif
#endif
#if KICKOS_FS_MODE == 6 && !defined(__thumb__)
#error "KICKOS_FS_MODE 6 spells its SP move for armv7m only"
#endif
#if KICKOS_FS_MODE == 2
#if !defined(__riscv) && !defined(__arm__) && !defined(__thumb__) && !defined(__RX__) \
    && !defined(__aarch64__)
#error "KICKOS_FS_MODE 2 needs this ISA's spelling for moving SP; it must not be built here"
#endif
// BUILT ON armv8a, NEVER REGISTERED THERE: the sp this moves is SP_EL0 and the trap entry
// builds its frame through SP_EL1, which EL0 cannot write, so no wild sp reaches a frame and
// there is no stack-bounds refusal on that backend for this arm to witness.
#if !KICKOS_HAVE_STACK_LIMIT
    // Outside every thread stack, and inside the app's own granted data so the frame is really
    // WRITTEN rather than faulting a second time. The whole frame has to land inside it: the
    // M-profile entry frame is 8 words, 26 with an FP context; lx6's trap frame is 256 bytes,
    // the widest elsewhere.
#if defined(__thumb__) && defined(__ARM_FP)
    alignas(16) volatile unsigned char g_offstack[104];
#elif defined(__thumb__)
    alignas(16) volatile unsigned char g_offstack[32];
#else
    alignas(16) volatile unsigned char g_offstack[320];
#endif
#endif
#endif

#if (KICKOS_FS_MODE == 10 || KICKOS_FS_MODE == 11) && !defined(__RX__)
#error "KICKOS_FS_MODE 10 and 11 spell RX traps and the RX PSW; they must not be built here"
#endif
#if KICKOS_FS_MODE == 13 && !defined(__RX__)
#error "KICKOS_FS_MODE 13 parks the RX USP; it must not be built here"
#endif

#if KICKOS_FS_MODE == 11
    constexpr uint32_t FS_PSW_PM = 1u << 20;
    // I, U and PM set, IPL 0: what an unprivileged thread's PSW holds, condition flags aside.
    constexpr uint32_t FS_PSW_USER = 0x00130000u;
    constexpr uint32_t FS_PSW_FLAGS = 0x0000000Fu;
#if KICKOS_TLS
    constexpr uint32_t FS_FORGE_STACK = KICKOS_TLS_STRIDE;
#else
    constexpr uint32_t FS_FORGE_STACK = 4096;
#endif
    // The head arm's endpoint, delegated to the server and the client at the same index.
    constexpr kos_cap_t FS_FORGE_EP = KOS_SPAWN_DELEGATED_CAP0;

    uint32_t psw_now()
    {
        uint32_t psw = 0;
        __asm volatile("mvfc psw, %0" : "=r"(psw));
        return psw;
    }

    void verdict(char const* arm, bool supervisor)
    {
        char line[64];
        if (supervisor)
        {
            ksnprintf(line, sizeof(line), "[fs] [%s] SUPERVISOR\n", arm);
        }
        else
        {
            ksnprintf(line, sizeof(line), "[fs] [%s] USER\n", arm);
        }
        kos::print(line);
    }

    // Clears PM in every word of [base, base + size) that reads as a user PSW once `ignore` is
    // masked off, and answers how many it rewrote.
    uint32_t forge(void* base, uint32_t size, uint32_t ignore)
    {
        volatile uint32_t* const words = static_cast<volatile uint32_t*>(base);
        uint32_t forged = 0;
        for (uint32_t i = 0; i < size / 4u; i++)
        {
            if ((words[i] & ~ignore) == FS_PSW_USER)
            {
                words[i] = words[i] & ~FS_PSW_PM;
                forged++;
            }
        }
        return forged;
    }

    void forged_count(char const* arm, uint32_t forged)
    {
        char line[64];
        ksnprintf(line, sizeof(line), "[fs] [%s] forged %u word(s)\n", arm,
                  static_cast<unsigned>(forged));
        kos::print(line);
    }

    // The return address the client's trap lands on, which the server sends back as the PC.
    uint32_t volatile g_head_pc = 0;
    constexpr uint32_t FS_HEAD_PSW = FS_PSW_USER & ~FS_PSW_PM;
    constexpr uint32_t FS_HEAD_REPLY = 8;

    void head_server(void*)
    {
        uint32_t reply[FS_HEAD_REPLY / 4u] = {};
        kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, FS_FORGE_EP, 0, KOS_TIMEOUT_NONE);
        int const rc = kos_reply_recv(KOS_CAP_NONE, reply, kos_call_lens_pack(0, sizeof(reply)),
                                      &opts);
        if (rc < 0 or opts.info.reply_cap == KOS_CAP_NONE)
        {
            kos::print("[fs] ERROR: the head server received no call\n");
            return;
        }
        reply[0] = g_head_pc;
        reply[1] = FS_HEAD_PSW;
        int const replied = kos_reply(opts.info.reply_cap, reply, sizeof(reply));
        if (replied != 0)
        {
            char line[64];
            ksnprintf(line, sizeof(line), "[fs] ERROR: the head reply was refused (%d)\n", replied);
            kos::print(line);
        }
    }

    // A KOS_SYS_CALL trapped with R0 at an address of this frame, its receive buffer the eight
    // bytes below it: where a trap that staged [PC][PSW] under the USP would have put them.
    void head_client(void*)
    {
        alignas(8) uint32_t area[32] = {};
        uint32_t* const usp = &area[24];
        uint32_t psw = 0;
        int32_t rc = 0;
        __asm volatile("mov.l   r0, r6\n\t"
                       "mov.l   #1f, r5\n\t"
                       "mov.l   r5, [%[pc]]\n\t"
                       "mov.l   %[usp], r0\n\t"
                       "mov.l   #-1, 4[r0]\n\t" // a3, KOS_TIMEOUT_NONE
                       "mov.l   %[ep], r2\n\t"
                       "mov.l   r0, r3\n\t"
                       "sub     #8, r3\n\t"
                       "mov.l   %[lens], r4\n\t"
                       "mov.l   #46, r1\n\t"    // KOS_SYS_CALL
                       "int     #1\n"
                       "1:\n\t"
                       "mov.l   r6, r0\n\t"
                       "mov.l   r1, %[rc]\n\t"
                       "mvfc    psw, %[psw]\n\t"
                       : [psw] "=&r"(psw), [rc] "=&r"(rc)
                       : [usp] "r"(usp), [pc] "r"(&g_head_pc), [ep] "r"(FS_FORGE_EP),
                         [lens] "r"(kos_call_lens_pack(0, FS_HEAD_REPLY))
                       : "r1", "r2", "r3", "r4", "r5", "r6", "r14", "r15", "memory");
        // A call refused before the reply reached area[22..23] returns too, still in user mode.
        if (rc != static_cast<int32_t>(FS_HEAD_REPLY) or area[22] != g_head_pc
            or area[23] != FS_HEAD_PSW)
        {
            char line[96];
            ksnprintf(line, sizeof(line), "[fs] ERROR: the head reply was not delivered (%ld)\n",
                      static_cast<long>(rc));
            kos::print(line);
            return;
        }
        verdict("head", (psw & FS_PSW_PM) == 0);
    }

    void first_child(void*)
    {
        verdict("first", (psw_now() & FS_PSW_PM) == 0);
    }

    bool volatile g_preempt_stop = false;

    void preempt_child(void*)
    {
        bool supervisor = false;
        do
        {
            if ((psw_now() & FS_PSW_PM) == 0)
            {
                supervisor = true;
            }
        } while (not g_preempt_stop);
        // Once more: the resume may land after the last test of the loop.
        if ((psw_now() & FS_PSW_PM) == 0)
        {
            supervisor = true;
        }
        verdict("preempt", supervisor);
    }

    bool spawn(void (*entry)(void*), char const* name, uint8_t prio, void* stack,
               kos_cap_grant const* caps, uint8_t cap_count, kos_thread_t* out)
    {
        kos_thread_params p = {};
        p.entry = entry;
        p.name = name;
        p.prio = prio;
        p.policy = KOS_POLICY_FIFO;
        p.caps = caps;
        p.cap_count = cap_count;
        if (stack != nullptr)
        {
            p.stack_base = stack;
            p.stack_size = FS_FORGE_STACK;
        }
        int const rc = kos_thread_create(&p, out);
        if (rc != 0)
        {
            char line[80];
            ksnprintf(line, sizeof(line), "[fs] ERROR: the %s thread was refused (%d)\n", name, rc);
            kos::print(line);
            return false;
        }
        return true;
    }

    // A stack this task reserved and reaches, which the thread it is handed to runs on.
    void* shared_stack()
    {
        void* const stack = kos_ram_alloc(FS_FORGE_STACK);
        if (stack == nullptr or kos_mem_self_grant(stack, FS_FORGE_STACK, 0) != 0)
        {
            kos::print("[fs] ERROR: no stack the forger can write\n");
            return nullptr;
        }
        return stack;
    }

    // Each arm in a thread of its own, so a thread one arm leaves supervisor runs nothing else.
    void forger()
    {
        kos::print("[fs] forger running\n");

        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            kos::print("[fs] ERROR: no endpoint for the head arm\n");
            return;
        }
        kos_cap_grant const server_caps[] = {{ep, KOS_CAP_WAIT}};
        kos_cap_grant const client_caps[] = {{ep, KOS_CAP_SIGNAL}};
        kos_thread_t server = 0;
        kos_thread_t client = 0;
        if (not spawn(head_server, "fs_server", 9, nullptr, server_caps, 1, &server)
            or not spawn(head_client, "fs_client", 8, nullptr, client_caps, 1, &client))
        {
            return;
        }
        (void)kos_thread_join(server, KOS_TIMEOUT_NONE);
        (void)kos_thread_join(client, KOS_TIMEOUT_NONE);

        // Created below this thread's priority, so it has not run when the forge reads its stack.
        void* stack = shared_stack();
        kos_thread_t child = 0;
        if (stack == nullptr or not spawn(first_child, "fs_first", 5, stack, nullptr, 0, &child))
        {
            return;
        }
        forged_count("first", forge(stack, FS_FORGE_STACK, 0));
        (void)kos_thread_join(child, KOS_TIMEOUT_NONE);

        // Spinning in user mode when this thread's wake preempts it, so its context is saved.
        stack = shared_stack();
        if (stack == nullptr or not spawn(preempt_child, "fs_preempt", 5, stack, nullptr, 0, &child))
        {
            return;
        }
        kos_sleep_ns(2000000ull);
        forged_count("preempt", forge(stack, FS_FORGE_STACK, FS_PSW_FLAGS));
        g_preempt_stop = true;
        (void)kos_thread_join(child, KOS_TIMEOUT_NONE);
    }
#endif

#if KICKOS_FS_MODE == 7
#if !KICKOS_HAVE_STACK_LIMIT || !KICKOS_MEMORY_ENFORCED
#error "KICKOS_FS_MODE 7 is the MPU's stacking abort on a core with a stack limit"
#endif
#endif

#if KICKOS_FS_MODE == 1
    volatile unsigned g_sink = 0;

    // `prev` forces one live frame per level: with the caller's array address escaping into the
    // call, at -Os neither inlining nor tail-recursion elimination can flatten this to a loop.
    __attribute__((noinline)) unsigned burn(unsigned depth, volatile unsigned* prev)
    {
        volatile unsigned pad[64];
        for (unsigned i = 0; i < 64; i++)
        {
            pad[i] = depth + i;
        }
        if (prev != nullptr)
        {
            pad[0] = prev[63];
        }
        if (depth == 0)
        {
            return pad[0];
        }
        return pad[63] + burn(depth - 1, pad);
    }
#endif

#if KICKOS_FS_MODE == 8 || KICKOS_FS_MODE == 9
#if KICKOS_FS_MODE == 8
    using fs_div_t = int32_t;
#else
    using fs_div_t = int64_t;
#endif

    // UBSan would report the division before it reached the hardware or the run-time ABI, which
    // are what this arm judges.
    __attribute__((noinline, no_sanitize("integer-divide-by-zero"))) fs_div_t divide_by_zero()
    {
        fs_div_t volatile dividend = 7;
        fs_div_t volatile divisor = 0;
        return dividend / divisor;
    }
#endif

#if KICKOS_FS_MODE == 12
    // Below the forger's own priority, so the child stays READY until the forger blocks.
    constexpr uint8_t FS_FORGE_CHILD_PRIO = 5;
    constexpr uint32_t FS_MSTATUS_MPP_M = 3u << 11;

    // Every frame the kernel left in [lo, hi): its pc slot is the child's entry and its sp slot
    // a 16-aligned address inside the block. Plants MPP=M in each and answers how many.
    unsigned plant(uintptr_t lo, uintptr_t hi, uint32_t entry)
    {
        unsigned n = 0;
        for (uintptr_t b = lo; b + KICKOS_RV_TRAP_FRAME <= hi; b += 4u)
        {
            volatile uint32_t* const f = reinterpret_cast<volatile uint32_t*>(b);
            uint32_t const sp = f[KICKOS_RV_TRAP_F_SP / 4];
            if (f[KICKOS_RV_TRAP_F_MEPC / 4] == entry and (sp & 15u) == 0 and sp > lo
                and sp <= hi)
            {
                uint32_t const mstatus = f[KICKOS_RV_TRAP_F_MSTATUS / 4];
                f[KICKOS_RV_TRAP_F_MSTATUS / 4] = mstatus | FS_MSTATUS_MPP_M;
                n++;
            }
        }
        return n;
    }

    void forge()
    {
        void* const block = kos_ram_alloc(FS_STACK_SIZE);
        if (block == nullptr or kos_mem_self_grant(block, FS_STACK_SIZE, 0) != 0)
        {
            kos::print("[fs] ERROR: the forger cannot reach a block of its own\n");
            return;
        }
        uintptr_t const lo = reinterpret_cast<uintptr_t>(block);
        uintptr_t const entry = reinterpret_cast<uintptr_t>(&fs_forge_child);
        kos_thread_params p = {};
        p.entry = fs_forge_child;
        p.name = "forged";
        p.prio = FS_FORGE_CHILD_PRIO;
        p.stack_base = block;
        p.stack_size = FS_STACK_SIZE;
        kos_thread_t child = KOS_THREAD_NONE;
        int const rc = kos_thread_create(&p, &child);
        // Nothing that can block runs before the plant: the child would run first.
        unsigned planted = 0;
        if (rc == 0)
        {
            planted = plant(lo, lo + FS_STACK_SIZE, static_cast<uint32_t>(entry));
        }
        char line[96];
        if (rc != 0)
        {
            ksnprintf(line, sizeof(line), "[fs] ERROR: the child on the forger's block: %d\n", rc);
            kos::print(line);
            return;
        }
        ksnprintf(line, sizeof(line), "[fs] [forge] child entry 0x%lx\n",
                  static_cast<unsigned long>(entry));
        kos::print(line);
        if (planted != 0)
        {
            kos::print("[fs] [forge] first frame on the child's user stack, MPP planted\n");
        }
        else
        {
            kos::print("[fs] [forge] no first frame on the child's user stack\n");
        }
        (void)kos_thread_join(child, KOS_TIMEOUT_NONE);
    }
#endif

    // The address main's argument names: the word mode 3 and 13 aim at, the stack's low edge of
    // mode 4, or the top of the block mode 2 and 7 aim at.
    uintptr_t g_fs_arg = 0;

#if KICKOS_FS_MODE == 13
    // Below the faulter's own priority, so the faulter's every wake preempts it.
    constexpr uint8_t FS_WILD_PRIO = 5;
    constexpr uint64_t FS_WILD_WAKES = 1000;

    // The USP is never restored: nothing but the switcher's save may touch it. USP = &word + 4
    // puts the save's first word, R15, on main's word, and R15 is not main's poison.
    [[noreturn]] void wild_spin(void*)
    {
        __asm volatile("mov.l   #0xC0DEBEEF, r15\n\t"
                       "mov.l   %0, r0\n"
                       "1:\n\t"
                       "bra     1b\n\t"
                       : : "r"(g_fs_arg + 4u) : "r15", "memory");
        __builtin_unreachable();
    }

    // Containment slays the spinner alone, so this thread outlives it and joins it. Each join
    // timeout is a wake that preempts the spinner.
    bool wildspin()
    {
        kos_thread_params p = {};
        p.entry = wild_spin;
        p.name = "fs_wild";
        p.prio = FS_WILD_PRIO;
        p.policy = KOS_POLICY_FIFO;
        kos_thread_t spinner = KOS_THREAD_NONE;
        int rc = kos_thread_create(&p, &spinner);
        if (rc != 0)
        {
            char line[64];
            ksnprintf(line, sizeof(line), "[fs] ERROR: the spinner was refused (%d)\n", rc);
            kos::print(line);
            return false;
        }
        for (uint64_t i = 0; i < FS_WILD_WAKES; i++)
        {
            rc = kos_thread_join(spinner, 1000u);
            if (rc == 0)
            {
                kos::print("[fs] [wildspin] the spinner is gone, its task runs on\n");
                return true;
            }
            if (rc != -KOS_ETIMEDOUT)
            {
                char line[64];
                ksnprintf(line, sizeof(line), "[fs] ERROR: the spinner's join failed (%d)\n", rc);
                kos::print(line);
                return false;
            }
        }
        return false;
    }
#endif

#if KICKOS_FS_MODE != 11
    void faulter()
    {
        kos::print("[fs] worker about to fault\n");
#if KICKOS_FS_MODE == 8 || KICKOS_FS_MODE == 9
        // Printed where the core returns a quotient instead of faulting, which the judge reads.
        char line[64];
        ksnprintf(line, sizeof(line), "[fs] divide by zero returned %lld\n",
                  static_cast<long long>(divide_by_zero()));
        kos::print(line);
        return;
#elif KICKOS_FS_MODE == 1
        g_sink = burn(4096, nullptr); // deeper than any thread stack in the tree
#elif KICKOS_FS_MODE == 2
        // Top of the buffer: the frame is pushed DOWNWARDS from here.
#if KICKOS_HAVE_STACK_LIMIT
        uintptr_t const top = g_fs_arg;
#else
        uintptr_t const top = reinterpret_cast<uintptr_t>(&g_offstack[sizeof(g_offstack)]);
#endif
#if defined(__riscv)
        __asm volatile("mv sp, %0\n\t" KOS_TRAP_ILLEGAL_INSN ::"r"(top) : "memory");
#elif defined(__RX__)
        // R0 IS the SP on RX.
        __asm volatile("mov.l %0, r0\n\t" KOS_TRAP_ILLEGAL_INSN ::"r"(top) : "memory");
#else
        __asm volatile("mov sp, %0\n\t" KOS_TRAP_ILLEGAL_INSN ::"r"(top) : "memory");
#endif
#elif KICKOS_FS_MODE == 3
        // A prologue that stores through the U-mode SP writes INTO main's word. The extent test
        // refuses the SP first, and the frame base is ctx.kernel_sp and no function of the SP
        // anyway. The worker takes the word's ADDRESS only and never writes it itself.
        uintptr_t const kw = g_fs_arg;
#if defined(__riscv)
        // trap_entry saves EVERY trap through the same prologue, so an illegal instruction
        // reaches it. The SP below is where a prologue building the frame at sp minus the frame
        // size would put the s2 slot (F_S2) exactly on main's word.
        static_assert(KICKOS_RV_TRAP_F_S2 < KICKOS_RV_TRAP_FRAME,
                      "the s2 slot must lie inside the frame, or this arm aims above main's "
                      "word and stops testing the prologue");
        __asm volatile("li s2, 0xC0DEBEEF\n\t"
                       "mv sp, %0\n\t" KOS_TRAP_ILLEGAL_INSN
                       : : "r"(kw + (KICKOS_RV_TRAP_FRAME - KICKOS_RV_TRAP_F_S2)) : "s2", "memory");
#elif defined(__RX__)
        // `int #1` (kickos_rx_syscall_trap) and not the fault path: on RX the fault path already
        // checks the USP, and the syscall trap is the entry that may store below it. USP =
        // &word + 8 puts the words just under the USP on main's word. R0 IS the SP on RX.
        __asm volatile("mov.l %0, r0\n\t"
                       "int #1\n\t"
                       : : "r"(kw + 8u) : "memory");
#endif
#elif KICKOS_FS_MODE == 4
        // The SP move and the trap are one statement, so an intact band names the kernel and
        // nothing else.
        uintptr_t const low = g_fs_arg + FS_LOW_ROOM;
        __asm volatile("mv sp, %0\n\t" KOS_TRAP_ILLEGAL_INSN ::"r"(low) : "memory");
#elif KICKOS_FS_MODE == 5
        // Two bytes down: bounds and extent both pass and only alignment can refuse it. sp is a
        // multiple of 4 at every instruction boundary, so sp - 2 cannot land aligned by
        // accident.
#if defined(__riscv)
        // Every trap goes through trap_entry, so the illegal instruction meets the same
        // alignment leg an ecall would.
        __asm volatile("addi sp, sp, -2\n\t" KOS_TRAP_ILLEGAL_INSN ::: "memory");
#elif defined(__RX__)
        // int #1, NOT the fault path: RX's alignment leg lives in the syscall trap and the SWINT
        // switcher, while a fault reaches kickos_fault_frame_trusted, which tests range and
        // extent but not alignment. R0 IS the SP on RX.
        __asm volatile("sub #2, r0\n\t"
                       "int #1\n\t"
                       : : : "memory");
#endif
#elif KICKOS_FS_MODE == 6
        __asm volatile("mov sp, %0\n\t" KOS_TRAP_ILLEGAL_INSN ::"r"(KICKOS_FS_NOWHERE) : "memory");
#elif KICKOS_FS_MODE == 10
        // The order is fs.h's: brk last.
        switch (g_fs_arg)
        {
            case 0:
            {
                kos::print("[fs] trap int #5\n");
                __asm volatile("int #5" ::: "memory");
                break;
            }
            case 1:
            {
                kos::print("[fs] trap int #26\n");
                __asm volatile("int #26" ::: "memory");
                break;
            }
            case 2:
            {
                kos::print("[fs] trap int #27\n");
                __asm volatile("int #27" ::: "memory");
                break;
            }
            case 3:
            {
                kos::print("[fs] trap int #30\n");
                __asm volatile("int #30" ::: "memory");
                break;
            }
            case 4:
            {
                kos::print("[fs] trap int #31\n");
                __asm volatile("int #31" ::: "memory");
                break;
            }
            case 5:
            {
                kos::print("[fs] trap int #32\n");
                __asm volatile("int #32" ::: "memory");
                break;
            }
            case 6:
            {
                kos::print("[fs] trap int #86\n");
                __asm volatile("int #86" ::: "memory");
                break;
            }
            case 7:
            {
                kos::print("[fs] trap int #87\n");
                __asm volatile("int #87" ::: "memory");
                break;
            }
            default:
            {
                kos::print("[fs] trap brk\n");
                __asm volatile("brk" ::: "memory");
                break;
            }
        }
        static_assert(FS_SWTRAP_COUNT == 9, "the switch above names nine traps");
#elif KICKOS_FS_MODE == 7
        __asm volatile("mov sp, %0\n\t" KOS_TRAP_ILLEGAL_INSN ::"r"(g_fs_arg) : "memory");
#elif KICKOS_FS_MODE == 12
        forge();
#elif KICKOS_FS_MODE == 13
        if (wildspin())
        {
            return;
        }
#else
        KOS_TRAP_ILLEGAL();
#endif
        kos::print("[fs] ERROR: worker did not fault\n");
    }
#endif
}

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        g_fs_arg = kickos::apps::hex_arg(argv[1]);
    }
#if KICKOS_FS_MODE == 11
    forger();
#else
    faulter();
#endif
    return 0;
}
