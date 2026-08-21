// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The fault-isolation witness: a thread that faults must die alone.
//
// KICKOS_FS_MODE 0: an unprivileged worker executes a trapping instruction; root must
// run AFTER it and end the system cleanly. The join is the ordering proof: it returns
// only once the worker is gone.
// KICKOS_FS_MODE 1: the worker recurses off its own stack (design 4.2). The thread
// produced no legitimate exception frame, so the fault must reach the PANIC dump instead
// of the thread kill. Needs a guarded stack, so an enforcing build gates it.
// KICKOS_FS_MODE 2: the worker points SP at a buffer OUTSIDE its stack and then faults.
// The frame is written in full, at a writable address, by a thread that really is
// unprivileged in thread mode, so every register-derived clause of the rule says yes and
// only the stack-bounds test can refuse it. It is the witness that a backend actually
// calls kickos_fault_frame_trusted, and the one mode 1 cannot stand in for: a stacking
// abort sets a status bit on armv7m, and this sets none anywhere.
// On RX the frame goes to the ISP instead, and what the bounds test reads is the USP the
// exit stub WOULD have run on.
// KICKOS_FS_MODE 3: the security regression for the software trap prologue. The worker
// points SP at a KERNEL word (kickos_trapstack_witness) and traps. A prologue that stored
// the frame through the U-mode SP overwrites that word in privileged mode; the bounds test
// refuses the SP before the first store.
// KICKOS_FS_MODE 4: the same defect one step in, where a FRAME-only bound still says yes.
// The worker runs on a CALLER-PROVIDED stack, so the app knows stack_lo and can poison a
// band of its own data immediately below it. The worker then parks SP inside its stack with
// room for the frame but not for the kernel descent under it, so a frame-only bound accepts
// it and the reporter chain runs privileged through the band. Root reads the band back after
// the join.
// KICKOS_FS_MODE 5: the alignment leg. The worker drops SP two bytes, still deep inside its
// own stack and in bounds, and traps, so only alignment can refuse it. QEMU virt COMPLETES
// the misaligned frame stores, so this mode witnesses the REFUSAL and not the prologue
// live-lock a trapping core would take instead.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/emit.h>

// An unset mode compares equal to 0, so it would silently build that arm.
#ifndef KICKOS_FS_MODE
#error "KICKOS_FS_MODE must be set by this app's CMakeLists"
#endif

using kickos::emit;

// The frame geometry modes 3 and 4 aim at. rv32imac only: this app's CMakeLists puts that
// backend's include directory on those two targets alone.
#if (KICKOS_FS_MODE == 3 || KICKOS_FS_MODE == 4) && defined(__riscv)
#include <kickos/arch/rv_trap_stack.h>
#endif

#if KICKOS_FS_MODE == 3
// A kernel .data word (kernel/init/fault.cc, selftest builds). Referenced for its ADDRESS
// only; an unprivileged thread that dereferenced it would fault.
extern "C" volatile unsigned kickos_trapstack_witness;
#endif

// The ISA's spelling for a synchronous fault the running instruction owns. The sim
// reaches the rule through SIGILL, not a guest trap.
#if defined(__riscv)
#define KICKOS_FS_TRAP() __asm volatile(".word 0x00000000") // illegal on RV32
#elif defined(__arm__) || defined(__thumb__)
#define KICKOS_FS_TRAP() __asm volatile("udf #0")
#elif defined(__RX__)
// A PRIVILEGED instruction: MVTIPL in user mode is a defined privileged-instruction
// exception (RXv3 ISA UM sec.5.1.2, and its own page). IPL is already 0, so an execution
// that lands in supervisor mode changes nothing and the app reports its own failure.
#define KICKOS_FS_TRAP() __asm volatile("mvtipl #0")
#else
#define KICKOS_FS_TRAP() __builtin_trap() // host: x86 ud2 -> SIGILL
#endif

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
    // Room the worker leaves below its SP: EXACTLY the frame, which is all a bound that
    // priced the frame ALONE demands. The frame then lands flush with stack_lo, in bounds, so
    // the frame-validity test accepts it, and every byte the kernel's C dispatch touches
    // under it is below stack_lo, where the band is.
    constexpr uintptr_t FS_LOW_ROOM = KICKOS_RV_TRAP_FRAME;
    static_assert(FS_LOW_ROOM >= KICKOS_RV_TRAP_FRAME,
                  "FS_LOW_ROOM below the frame size: the frame lands out of bounds and the "
                  "frame-validity test refuses it, so the arm no longer shows the descent");
    static_assert(FS_LOW_ROOM < KICKOS_RV_TRAP_REDZONE,
                  "FS_LOW_ROOM at or above the red zone: the fixed guard accepts this SP and "
                  "the arm can never go green");

    // The caller-owned stack, and the band of the app's own arena block directly beneath it.
    // Size clears KICKOS_MIN_STACK_SIZE and exceeds the syscall red zone, so the worker is a
    // legitimate thread until the moment it moves SP. Under an enforcing MPU the stack is one
    // region, so PMP NAPOT wants the base aligned to its size; main checks that it got it.
    constexpr uint32_t FS_STACK_SIZE = 2048;
    constexpr uint32_t FS_BAND_SIZE = 512;
    constexpr uint32_t FS_BAND_POISON = 0x5AFEBA5Eu;

    uintptr_t g_fs_stack_lo = 0; // published before the spawn
#endif
#if KICKOS_FS_MODE == 5
#if !defined(__riscv) && !defined(__RX__)
#error "KICKOS_FS_MODE 5 needs this ISA's spelling for moving SP; it must not be built here"
#endif
#endif
#if KICKOS_FS_MODE == 2
#if !defined(__riscv) && !defined(__arm__) && !defined(__thumb__) && !defined(__RX__)
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
    // escaping into the call the frame cannot be reused, so at -Os neither inlining nor
    // the accumulator form of tail-recursion elimination can flatten this into a single
    // frame and a loop that never leaves the stack region.
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
#elif defined(__RX__)
        __asm volatile("mov.l %0, r0" ::"r"(top) : "memory"); // R0 IS the SP on RX
#else
        __asm volatile("mov sp, %0" ::"r"(top) : "memory");
#endif
        KICKOS_FS_TRAP();
#elif KICKOS_FS_MODE == 3
        // Aim the trap's frame at the kernel witness and trap. A prologue that stores
        // through the U-mode SP writes INTO kernel .data; the bounds test refuses the SP
        // first. Nothing may run between the SP move and the trap.
        uintptr_t const kw = reinterpret_cast<uintptr_t>(&kickos_trapstack_witness);
#if defined(__riscv)
        // trap_entry saves EVERY trap (ecall/interrupt/fault) through the same prologue, so
        // an illegal instruction reaches it. The prologue builds the frame at sp minus the
        // frame size and stores s2 at F_S2 within it, so the store lands on the witness when
        // sp sits FRAME - F_S2 above it.
        static_assert(KICKOS_RV_TRAP_F_S2 < KICKOS_RV_TRAP_FRAME,
                      "the s2 slot must lie inside the frame, or this arm aims above the "
                      "witness and stops testing the prologue");
        __asm volatile("li s2, 0xC0DEBEEF\n\t"
                       "mv sp, %0\n\t"
                       : : "r"(kw + (KICKOS_RV_TRAP_FRAME - KICKOS_RV_TRAP_F_S2)) : "s2", "memory");
        KICKOS_FS_TRAP();
#elif defined(__RX__)
        // The RX hole is in the SYSCALL trap and the switcher, not the fault path, which
        // already checks the USP. So enter through `int #1` (kickos_rx_syscall_trap): its
        // generic arm stores the stacked userPC/userPSW at USP-8/USP-4, so USP = &witness + 8
        // lands userPC (non-sentinel) on the witness. R0 IS the SP on RX.
        __asm volatile("mov.l %0, r0\n\t"
                       "int #1\n\t"
                       : : "r"(kw + 8u) : "memory");
#endif
#elif KICKOS_FS_MODE == 4
        // Nothing may run between the SP move and the trap: what follows in the frame's
        // shadow must be the kernel's.
        uintptr_t const low = g_fs_stack_lo + FS_LOW_ROOM;
        __asm volatile("mv sp, %0" ::"r"(low) : "memory");
        KICKOS_FS_TRAP();
#elif KICKOS_FS_MODE == 5
        // Two bytes down: still deep inside this thread's own stack and far above its bottom,
        // so bounds and extent both pass and only alignment can refuse it. sp is a multiple of
        // 4 at every instruction boundary, so sp - 2 cannot land aligned by accident.
        // Nothing may run between the SP move and the trap.
#if defined(__riscv)
        // Every trap goes through trap_entry, so the deliberate illegal instruction meets
        // the same alignment leg an ecall would.
        __asm volatile("addi sp, sp, -2" ::: "memory");
        KICKOS_FS_TRAP();
#elif defined(__RX__)
        // int #1, NOT the fault path: RX's alignment leg lives in the syscall trap and the
        // SWINT switcher, and a fault instead reaches kickos_fault_frame_trusted, which
        // tests range and extent but not alignment and so credits the thread cleanly.
        // R0 IS the SP on RX.
        __asm volatile("sub #2, r0\n\t"
                       "int #1\n\t"
                       : : : "memory");
#endif
#else
        KICKOS_FS_TRAP();
#endif
        emit("[fs] ERROR: worker did not fault\n");
    }
}

int main(int, char**)
{
    void* stack = nullptr;
    uint32_t stack_size = 0;
#if KICKOS_FS_MODE == 4
    // One arena block laid out as [ padding ][ band ][ thread stack ], the band ending
    // exactly at stack_lo. kos_ram_alloc rounds the request to a describable region size and
    // returns it aligned to that, so asking for twice the stack puts stack_lo one stack-size
    // into a block aligned to twice it: the stack is nameable by one PMP entry and the band
    // fits underneath. The band is NOT granted to the worker; only the kernel can reach it.
    void* const raw = kos_ram_alloc(2u * FS_STACK_SIZE);
    if (raw == nullptr)
    {
        emit("[fs] ERROR: the arena cannot spare a caller-owned stack for this arm\n");
        return 1;
    }
    uintptr_t const lo = reinterpret_cast<uintptr_t>(raw) + FS_STACK_SIZE;
    if ((lo & (FS_STACK_SIZE - 1u)) != 0)
    {
        emit("[fs] ERROR: the arena block is not aligned to the stack size\n");
        return 1;
    }
    // kos_ram_alloc reserves and grants nothing, so root has to ask for the band before it
    // can poison or read it. The BAND only, never the whole block: the stack half becomes a
    // kernel-reserved region at the spawn below and the same admission predicate would then
    // refuse it.
    if (kos_mem_self_grant(reinterpret_cast<void*>(lo - FS_BAND_SIZE), FS_BAND_SIZE, 0) != 0)
    {
        emit("[fs] ERROR: root cannot reach the band it has to read back\n");
        return 1;
    }
    volatile uint32_t* const band = reinterpret_cast<volatile uint32_t*>(lo - FS_BAND_SIZE);
    for (uint32_t i = 0; i < FS_BAND_SIZE / 4u; i++)
    {
        band[i] = FS_BAND_POISON;
    }
    g_fs_stack_lo = lo;
    stack = reinterpret_cast<void*>(lo);
    stack_size = FS_STACK_SIZE;
#endif
    emit("[fs] spawning the faulter\n");
    kos::thread::Handle t = kos::thread::spawn(faulter, nullptr, "faulter", 10,
                                               KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                               nullptr, 0, stack, stack_size);
    int const rc = t.join(KOS_TIMEOUT_NONE);
    emit("[fs] survivor ran after the fault\n");
#if KICKOS_FS_MODE == 4
    // Reachable only when the fault was survivable, which is itself the failure this arm
    // reports: the refusal ends the system and root never runs at all.
    bool corrupt = false;
    for (uint32_t i = 0; i < FS_BAND_SIZE / 4u; i++)
    {
        if (band[i] != FS_BAND_POISON)
        {
            corrupt = true;
        }
    }
    if (corrupt)
    {
        emit("[fs] [lowband] CORRUPTED: the kernel ran below stack_lo on a U-mode sp\n");
    }
#endif
    if (rc != 0)
    {
        emit("[fs] ERROR: join did not report the worker gone\n");
        return 1;
    }
    return 0;
}
