// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The fault-isolation witness: a fault must be contained to the faulting thread's TASK, and
// the system must outlive it. The victim is spawned into a task of its OWN, which is what
// makes the containment observable: root is the survivor and shares no space with it.
//
// KICKOS_FS_MODE
//   0  an unprivileged worker executes a trapping instruction; root must run AFTER it and end
//      the system cleanly. The join is the ordering proof.
//   1  the worker recurses off its own stack, producing no legitimate exception frame, so the
//      fault must reach the PANIC dump and not the thread kill. Needs a guarded stack.
//   2  the worker points SP at a buffer OUTSIDE its stack and faults. The frame is written in
//      full, at a writable address, by a thread that really is unprivileged in thread mode, so
//      every register-derived clause of the rule says yes and only the stack-bounds test can
//      refuse it. On RX the frame goes to the ISP instead and the bounds test reads the USP the
//      exit stub WOULD have run on.
//   3  the worker points SP at a KERNEL word (kickos_trapstack_witness) and traps. A software
//      trap prologue that stored the frame through the U-mode SP overwrites that word in
//      privileged mode; the bounds test refuses the SP before the first store.
//   4  the LOW EDGE, which is legal. The worker runs on a CALLER-PROVIDED stack, so the app
//      knows stack_lo and poisons a band of its own data immediately below it; the worker then
//      parks SP with less room under it than the frame plus the kernel descent needs, and
//      faults. On rv32imac that SP is accepted, the entry transferring to the thread's kernel
//      stack, so the claim is a clean kill with the band INTACT. An entry that adopted the
//      U-mode SP would run the reporter chain privileged through the band.
//   5  the alignment leg. The worker drops SP two bytes, still deep inside its own stack and in
//      bounds, so only alignment can refuse it. QEMU virt COMPLETES the misaligned frame stores,
//      so this witnesses the REFUSAL and not the prologue live-lock a trapping core would take.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/emit.h>

// An unset mode compares equal to 0, so it would silently build that arm.
#ifndef KICKOS_FS_MODE
#error "KICKOS_FS_MODE must be set by this app's CMakeLists"
#endif

using kickos::emit;

// rv32imac only: this app's CMakeLists puts that backend's include directory on modes 3 and 4
// alone.
#if (KICKOS_FS_MODE == 3 || KICKOS_FS_MODE == 4) && defined(__riscv)
#include <kickos/arch/rv_trap_stack.h>
#endif

#if KICKOS_FS_MODE == 3
// A kernel .data word (kernel/init/fault.cc, selftest builds), referenced for its ADDRESS only:
// an unprivileged thread that dereferenced it would fault.
extern "C" volatile unsigned kickos_trapstack_witness;
#endif

// A synchronous fault the running instruction owns. The sim reaches the rule through SIGILL.
#if defined(__riscv)
#define KICKOS_FS_TRAP() __asm volatile(".word 0x00000000") // illegal on RV32
#elif defined(__arm__) || defined(__thumb__)
#define KICKOS_FS_TRAP() __asm volatile("udf #0")
#elif defined(__RX__)
// MVTIPL in user mode is a defined privileged-instruction exception (RXv3 ISA UM sec.5.1.2).
// IPL is already 0, so an execution that lands in supervisor mode changes nothing.
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
    // EXACTLY the frame, so an entry that built the frame at the parked SP lands it flush with
    // stack_lo and runs every byte of its C dispatch below stack_lo, where the band is.
    constexpr uintptr_t FS_LOW_ROOM = KICKOS_RV_TRAP_FRAME;
    static_assert(FS_LOW_ROOM < KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_KERNEL_DEPTH,
                  "FS_LOW_ROOM leaves room for the frame and the kernel descent, so even an "
                  "entry that adopted this SP would stay above stack_lo and an intact band "
                  "would prove nothing");

    // The size clears KICKOS_MIN_STACK_SIZE, so the worker is a legitimate thread. Under an
    // enforcing MPU the stack is one region, so PMP NAPOT wants the base aligned to its size;
    // main checks that it got it.
    constexpr uint32_t FS_STACK_SIZE = 2048;
    constexpr uint32_t FS_BAND_SIZE = 512;
    constexpr uint32_t FS_BAND_POISON = 0x5AFEBA5Eu;

    uintptr_t g_fs_stack_lo = 0;
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
    // Outside every thread stack, and inside the app's own granted data so the frame is really
    // WRITTEN rather than faulting a second time. The whole frame has to land inside it: armv7m
    // may stack an FP frame of 104 bytes and the RISC-V trap prologue pushes 128.
    alignas(16) volatile unsigned char g_offstack[512];
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

    void faulter(void*)
    {
        emit("[fs] worker about to fault\n");
#if KICKOS_FS_MODE == 1
        g_sink = burn(4096, nullptr); // deeper than any thread stack in the tree
#elif KICKOS_FS_MODE == 2
        // Top of the buffer: the frame is pushed DOWNWARDS from here. Nothing may run between
        // the SP move and the trap.
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
        // A prologue that stores through the U-mode SP writes INTO kernel .data. The extent test
        // refuses the SP first, and the frame base is ctx.kernel_sp and no function of the SP
        // anyway. Nothing may run between the SP move and the trap.
        uintptr_t const kw = reinterpret_cast<uintptr_t>(&kickos_trapstack_witness);
#if defined(__riscv)
        // trap_entry saves EVERY trap through the same prologue, so an illegal instruction
        // reaches it. The SP below is where a prologue building the frame at sp minus the frame
        // size would put the s2 slot (F_S2) exactly on the witness word.
        static_assert(KICKOS_RV_TRAP_F_S2 < KICKOS_RV_TRAP_FRAME,
                      "the s2 slot must lie inside the frame, or this arm aims above the "
                      "witness and stops testing the prologue");
        __asm volatile("li s2, 0xC0DEBEEF\n\t"
                       "mv sp, %0\n\t"
                       : : "r"(kw + (KICKOS_RV_TRAP_FRAME - KICKOS_RV_TRAP_F_S2)) : "s2", "memory");
        KICKOS_FS_TRAP();
#elif defined(__RX__)
        // `int #1` (kickos_rx_syscall_trap) and not the fault path: on RX the fault path already
        // checks the USP and the hole is in the syscall trap. Its generic arm stores the stacked
        // userPC/userPSW at USP-8/USP-4, so USP = &witness + 8 lands userPC on the witness.
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
        // Nothing may run between the SP move and the trap, so that an intact band names the
        // kernel and nothing else.
        uintptr_t const low = g_fs_stack_lo + FS_LOW_ROOM;
        __asm volatile("mv sp, %0" ::"r"(low) : "memory");
        KICKOS_FS_TRAP();
#elif KICKOS_FS_MODE == 5
        // Two bytes down: bounds and extent both pass and only alignment can refuse it. sp is a
        // multiple of 4 at every instruction boundary, so sp - 2 cannot land aligned by
        // accident. Nothing may run between the SP move and the trap.
#if defined(__riscv)
        // Every trap goes through trap_entry, so the illegal instruction meets the same
        // alignment leg an ecall would.
        __asm volatile("addi sp, sp, -2" ::: "memory");
        KICKOS_FS_TRAP();
#elif defined(__RX__)
        // int #1, NOT the fault path: RX's alignment leg lives in the syscall trap and the SWINT
        // switcher, while a fault reaches kickos_fault_frame_trusted, which tests range and
        // extent but not alignment. R0 IS the SP on RX.
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
    // One arena block laid out as [ padding ][ band ][ thread stack ], the band ending exactly
    // at stack_lo. kos_ram_alloc rounds the request to a describable region size and returns it
    // aligned to that, so asking for twice the stack puts stack_lo one stack-size into a block
    // aligned to twice it: the stack is nameable by one PMP entry and the band fits underneath.
    // The band is NOT granted to the worker.
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
    // The BAND only, never the whole block: the stack half becomes a kernel-reserved region at
    // the spawn below and the same admission predicate would then refuse it.
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
    // THE FAULTER GETS A TASK OF ITS OWN, and that is the arm rather than a detail of it. A
    // fault ends the faulting thread's whole task, and a plain spawn is a thread OF THE
    // CALLER'S task, so a victim spawned the plain way would take root with it and this would
    // witness the fault reaching root instead of being contained.
    kos_task_t victim = KOS_TASK_NONE;
    if (kos_task_create(nullptr, 0, 0, &victim) != 0)
    {
        emit("[fs] ERROR: no task slot for the faulter\n");
        return 1;
    }
    emit("[fs] spawning the faulter\n");
    kos::thread::Handle t = kos::thread::create(faulter, nullptr, "faulter", 10,
                                                KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                nullptr, 0, stack, stack_size,
                                                nullptr, 0, nullptr, 0, 0, nullptr, victim);
    int const rc = t.join(KOS_TIMEOUT_NONE);
    // Drops root's hold on a group that is already empty, so the slot goes back here rather
    // than at root's own exit.
    (void)kos_task_kill(victim);
    emit("[fs] survivor ran after the fault\n");
#if KICKOS_FS_MODE == 4
    // Both outcomes PRINT: a silent arm is indistinguishable from a deleted band check, a spawn
    // that never happened, or a worker that never faulted.
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
    else
    {
        emit("[fs] [lowband] INTACT: the kernel wrote nothing below the parked sp\n");
    }
#endif
    if (rc != 0)
    {
        emit("[fs] ERROR: join did not report the worker gone\n");
        return 1;
    }
    return 0;
}
