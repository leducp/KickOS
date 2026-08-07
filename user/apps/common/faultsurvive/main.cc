// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The fault-isolation witness: a thread that faults must die alone.
//
// KICKOS_FS_MODE 0: an unprivileged worker executes an undefined instruction; root must
// run AFTER it and end the system cleanly. The join is the ordering proof: it returns
// only once the worker is gone.
// KICKOS_FS_MODE 1: the worker recurses off its own stack (design 4.2). There is no
// exception frame the thread legitimately produced, so the fault must reach the PANIC
// dump instead of the thread kill. Needs a guarded stack, so only an enforcing build
// gates it.
// KICKOS_FS_MODE 2: the worker points SP at a buffer OUTSIDE its stack and then faults.
// The frame is written in full, at a writable address, by a thread that really is
// unprivileged in thread mode, so every register-derived clause of the rule says yes and
// only the stack-bounds test can refuse it. It is the witness that a backend actually
// calls kickos_fault_frame_trusted, and the one mode 1 cannot stand in for: a stacking
// abort sets a status bit on armv7m, and this sets none anywhere.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/emit.h>

// Undefined would silently select mode 0 rather than fail.
#ifndef KICKOS_FS_MODE
#error "KICKOS_FS_MODE must be set by this app's CMakeLists"
#endif

using kickos::emit;

// The ISA's undefined-instruction spelling. The sim reaches the rule through SIGILL, not
// a guest trap.
#if defined(__riscv)
#define KICKOS_FS_TRAP() __asm volatile(".word 0x00000000") // illegal on RV32
#elif defined(__arm__) || defined(__thumb__)
#define KICKOS_FS_TRAP() __asm volatile("udf #0")
#else
#define KICKOS_FS_TRAP() __builtin_trap() // host: x86 ud2 -> SIGILL
#endif

namespace
{
#if KICKOS_FS_MODE == 2
#if !defined(__riscv) && !defined(__arm__) && !defined(__thumb__)
#error "KICKOS_FS_MODE 2 needs this ISA's spelling for moving SP; it must not be built here"
#endif
    // Outside every thread stack by construction, and inside the app's own granted data
    // so the frame is really WRITTEN there rather than faulting a second time. Aligned
    // and oversized: armv7m may stack an FP frame of 104 bytes and the RISC-V trap
    // prologue pushes 128, and the whole frame has to land inside the buffer for the
    // fault to be the one this mode means to produce.
    alignas(16) volatile unsigned char g_offstack[512];
#endif

#if KICKOS_FS_MODE == 1
    volatile unsigned g_sink = 0;

    // `prev` is what forces one live frame per level: with the caller's array address
    // escaping into the call the frame cannot be reused, so neither inlining nor the
    // accumulator form of tail-recursion elimination can flatten this into a loop. A
    // shape without it compiled at -Os to a single 256-byte frame and a loop, and never
    // left the stack region.
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

    void faulter(void*)
    {
        emit("[fs] worker about to fault\n");
#if KICKOS_FS_MODE == 1
        g_sink = burn(4096, nullptr); // deeper than any thread stack in the tree
#elif KICKOS_FS_MODE == 2
        // Top of the buffer: the frame is pushed DOWNWARDS from here, so it lands inside
        // g_offstack. Nothing may run between the SP move and the trap; emit() above is
        // already done and the trap is the next instruction.
        uintptr_t const top = reinterpret_cast<uintptr_t>(&g_offstack[sizeof(g_offstack)]);
#if defined(__riscv)
        __asm volatile("mv sp, %0" ::"r"(top) : "memory");
#else
        __asm volatile("mov sp, %0" ::"r"(top) : "memory");
#endif
        KICKOS_FS_TRAP();
#else
        KICKOS_FS_TRAP();
#endif
        emit("[fs] ERROR: worker did not fault\n");
    }
}

int main(int, char**)
{
    emit("[fs] spawning the faulter\n");
    kos::thread::Handle t = kos::thread::spawn(faulter, nullptr, "faulter", 10,
                                               KOS_POLICY_FIFO, 0, /*privileged=*/false);
    int const rc = t.join(KOS_TIMEOUT_NONE);
    emit("[fs] survivor ran after the fault\n");
    if (rc != 0)
    {
        emit("[fs] ERROR: join did not report the worker gone\n");
        return 1;
    }
    return 0;
}
