// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// faultsurvive's faulter, a program of its own so that its fault ends a task main is not in.
// KICKOS_FS_MODE is main.cc's; the one argument, where the mode takes an address, is that
// address in hex.

#include <kickos/apps/peer_run.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/trap.h>

// An unset mode compares equal to 0, so it would silently build that arm.
#ifndef KICKOS_FS_MODE
#error "KICKOS_FS_MODE must be set by this app's CMakeLists"
#endif

// rv32imac only: this app's CMakeLists puts that backend's include directory on modes 3 and 4
// alone.
#if (KICKOS_FS_MODE == 3 || KICKOS_FS_MODE == 4) && defined(__riscv)
#include <kickos/arch/rv_trap_stack.h>
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

    // The address main's argument names: the word mode 3 aims at, the stack's low edge of mode 4,
    // or the top of the block mode 2 and 7 aim at.
    uintptr_t g_fs_arg = 0;

    void faulter()
    {
        kos::print("[fs] worker about to fault\n");
#if KICKOS_FS_MODE == 1
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
        // checks the USP and the hole is in the syscall trap. Its generic arm stores the stacked
        // userPC/userPSW at USP-8/USP-4, so USP = &word + 8 lands userPC on main's word.
        // R0 IS the SP on RX.
        //
        // That head survives the kernel-stack transfer: RX leaves supervisor only by RTE, RTE
        // pops PC then PSW from R0, and R0 is the USP while PSW.U is set, so the pair sits on
        // the stack the thread resumes on however far the dispatch moves.
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
#elif KICKOS_FS_MODE == 7
        __asm volatile("mov sp, %0\n\t" KOS_TRAP_ILLEGAL_INSN ::"r"(g_fs_arg) : "memory");
#else
        KOS_TRAP_ILLEGAL();
#endif
        kos::print("[fs] ERROR: worker did not fault\n");
    }
}

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        g_fs_arg = kickos::apps::hex_arg(argv[1]);
    }
    faulter();
    return 0;
}
