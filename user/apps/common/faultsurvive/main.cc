// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The fault-isolation witness: a fault must be contained to the faulting thread's TASK, and
// the system must outlive it. The victim is a program of its own (peer.cc), which is what
// makes the containment observable: main is the survivor and shares no space with it.
//
// KICKOS_FS_MODE (the peer takes it too, and the address a mode needs as its one argument)
//   0  an unprivileged worker executes a trapping instruction; main must run AFTER it and end
//      the system cleanly. The join is the ordering proof.
//   1  the worker recurses off its own stack, producing no legitimate exception frame, so the
//      fault must reach the PANIC dump and not the thread kill, except where the frame lies off
//      the thread's stack (RX: the ISP). Needs a guarded stack.
//   2  the worker points SP at a buffer OUTSIDE its stack and faults. The frame is written in
//      full, at a writable address, by a thread that really is unprivileged in thread mode, so
//      every register-derived clause of the rule says yes and only the stack-bounds test can
//      refuse it. On RX the frame goes to the ISP instead and the worker dies alone. Above the
//      stack where a stack limit refuses one below.
//   3  the worker points SP at a word of MAIN's data (its address is the peer's argument) and
//      traps. A software trap prologue that stored the frame through the U-mode SP overwrites
//      that word in privileged mode, where no protection applies; the bounds test refuses the SP
//      before the first store. Main reads the word back after the join and prints either verdict.
//   4  the LOW EDGE, which is legal. The worker runs on a CALLER-PROVIDED stack, so the app
//      knows stack_lo and poisons a band of its own data immediately below the block, under the
//      thread's libc state; the worker then parks SP with less room under it than the frame plus
//      the kernel descent needs, and faults. On rv32imac that SP is accepted, the entry
//      transferring to the thread's kernel stack, so the claim is a clean kill with the band
//      INTACT. An entry that adopted the U-mode SP would run the reporter chain privileged
//      through the band.
//   5  the alignment leg. The worker drops SP two bytes, still deep inside its own stack and in
//      bounds, so only alignment can refuse it. QEMU virt COMPLETES the misaligned frame stores,
//      so this witnesses the REFUSAL and not the prologue live-lock a trapping core would take.
//   6  the worker points SP where no memory answers (KICKOS_FS_NOWHERE) and traps: the
//      stacking aborts, and a privileged read of that frame would fault again in handler mode.
//   7  the overflow arm on a core with a stack limit, which stops a recursion first: SP aimed
//      at an arena block above the stack that the worker holds no grant on, so the MPU aborts
//      the stacking.
//   8  the worker divides by zero in 32 bits; 9 in 64. Where the core traps it, the worker is
//      killed as by any fault; where the ISA defines a quotient instead, the worker prints it.
//      Either way main must run after it.
//  10  RX only: the worker executes one software trap, an INT #n or BRK through an INTB slot that
//      is not the syscall's, once per spawn over FS_SWTRAP_COUNT spawns. Each must kill the worker
//      and nothing else, and none may move the clock: main reads it around the whole loop.
//  11  RX only: the worker forges a PSW with PM clear wherever a task can write one, from a
//      thread of its own per arm, and each target thread prints the mode it then runs in.
//  12  rv32imac only: the faulter takes a block, starts a lower-priority child on it, and plants
//      MPP=M in every frame of the child's it finds there before the child first runs. The
//      child's first instruction reads mstatus, so it must die from U-mode at its entry, never
//      print.
//  13  RX only: a thread of the faulter points its USP just above MAIN's word (as mode 3) and
//      spins until the faulter's wake preempts it. The switcher's guard must slay that thread
//      before the save stores through that USP, and the faulter, which outlives it, says so;
//      main reads the word back after the join.

#include <kickos/apps/peer_run.h>
#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>

#include "fs.h"

// An unset mode compares equal to 0, so it would silently build that arm.
#ifndef KICKOS_FS_MODE
#error "KICKOS_FS_MODE must be set by this app's CMakeLists"
#endif

namespace
{
#if KICKOS_FS_MODE == 3 || KICKOS_FS_MODE == 13
    constexpr unsigned FS_KWORD_POISON = 0x5AFEBA5Eu;
    volatile unsigned g_fs_kword = FS_KWORD_POISON;
#endif
}

#if KICKOS_FS_MODE == 4
// What the kernel carves off the worker's stack below its stack_lo (arch/common/sections.ld.h).
extern "C" char const __kickos_tls_carve[];
#endif

#if KICKOS_FS_MODE == 7 || (KICKOS_FS_MODE == 2 && KICKOS_HAVE_STACK_LIMIT)
namespace
{
    // Allocation order does not place a block: the arena fills an earlier alignment run-up
    // first. Of two equal blocks the lower is the worker's stack and the upper the block above.
    bool stack_and_above(void** stack, void** above)
    {
        void* const a = kos_ram_alloc(FS_OFF_STACK_SIZE);
        void* const b = kos_ram_alloc(FS_OFF_STACK_SIZE);
        if (a == nullptr or b == nullptr)
        {
            return false;
        }
        *stack = a;
        *above = b;
        if (reinterpret_cast<uintptr_t>(b) < reinterpret_cast<uintptr_t>(a))
        {
            *stack = b;
            *above = a;
        }
        return true;
    }
}
#endif

int main(int, char**)
{
    void* stack = nullptr;
    uint32_t stack_size = 0;
    void* block = nullptr;
    uint32_t block_size = 0;
    uintptr_t arg = 0;
#if KICKOS_FS_MODE == 3 || KICKOS_FS_MODE == 13
    arg = reinterpret_cast<uintptr_t>(&g_fs_kword);
#endif
#if KICKOS_FS_MODE == 4
    // One arena block laid out as [ padding ][ band ][ thread stack ], the band ending exactly
    // at the stack block, whose carve ends at stack_lo. kos_ram_alloc rounds the request to a
    // describable region size and returns it aligned to that, so asking for twice the stack
    // puts the stack block one stack-size into a block aligned to twice it: the stack is
    // nameable by one PMP entry and the band fits underneath. The band is NOT granted to the
    // worker.
    void* const raw = kos_ram_alloc(2u * FS_STACK_SIZE);
    if (raw == nullptr)
    {
        kos::print("[fs] ERROR: the arena cannot spare a caller-owned stack for this arm\n");
        return 1;
    }
    uintptr_t const lo = reinterpret_cast<uintptr_t>(raw) + FS_STACK_SIZE;
    if ((lo & (FS_STACK_SIZE - 1u)) != 0)
    {
        kos::print("[fs] ERROR: the arena block is not aligned to the stack size\n");
        return 1;
    }
    // The BAND only, never the whole block: the stack half becomes a kernel-reserved region at
    // the spawn below and the same admission predicate would then refuse it.
    if (kos_mem_self_grant(reinterpret_cast<void*>(lo - FS_BAND_SIZE), FS_BAND_SIZE, 0) != 0)
    {
        kos::print("[fs] ERROR: main cannot reach the band it has to read back\n");
        return 1;
    }
    volatile uint32_t* const band = reinterpret_cast<volatile uint32_t*>(lo - FS_BAND_SIZE);
    for (uint32_t i = 0; i < FS_BAND_SIZE / 4u; i++)
    {
        band[i] = FS_BAND_POISON;
    }
    arg = lo + reinterpret_cast<uintptr_t>(__kickos_tls_carve);
    stack = reinterpret_cast<void*>(lo);
    stack_size = FS_STACK_SIZE;
#endif
#if KICKOS_FS_MODE == 7
    void* above = nullptr;
    if (not stack_and_above(&stack, &above))
    {
        kos::print("[fs] ERROR: the arena cannot spare the worker's stack and the block above\n");
        return 1;
    }
    stack_size = FS_OFF_STACK_SIZE;
    arg = reinterpret_cast<uintptr_t>(above) + FS_OFF_STACK_SIZE;
#endif
#if KICKOS_FS_MODE == 2 && KICKOS_HAVE_STACK_LIMIT
    void* off = nullptr;
    if (not stack_and_above(&stack, &off))
    {
        kos::print("[fs] ERROR: the arena cannot spare the worker's stack and its buffer\n");
        return 1;
    }
    stack_size = FS_OFF_STACK_SIZE;
    block = off;
    block_size = FS_OFF_STACK_SIZE;
    arg = reinterpret_cast<uintptr_t>(off) + FS_OFF_STACK_SIZE;
#endif
    // THE FAULTER IS A PROGRAM OF ITS OWN: a fault ends the faulting thread's whole task, so a
    // victim in main's task would take main with it.
    kos_program_t prog = KOS_CAP_NONE;
    if (kos_program_find(KICKOS_FS_PEER, &prog) != 0)
    {
        kos::print("[fs] ERROR: no faulter program\n");
        return 1;
    }
    uint32_t runs = 1;
#if KICKOS_FS_MODE == 10
    runs = FS_SWTRAP_COUNT;
    uint64_t const clock_before = kos_clock_now();
    bool jumped = false;
#endif
    int rc = 0;
    for (uint32_t run = 0; run < runs; run++)
    {
#if KICKOS_FS_MODE == 10
        arg = run;
#endif
        char arg_text[24];
        ksnprintf(arg_text, sizeof(arg_text), "%p", reinterpret_cast<void*>(arg));
        char const* const argv[] = {"faulter", arg_text, nullptr};
        kos_task_params params = {};
        params.argv = argv;
        params.block = block;
        params.size = block_size;
        params.stack = stack;
        params.stack_size = stack_size;
        kos::print("[fs] spawning the faulter\n");
        kos_task_t victim = KOS_TASK_NONE;
        rc = kickos::apps::create_peer(prog, params, &victim);
        if (rc != 0)
        {
            kos::print("[fs] ERROR: no task slot for the faulter\n");
            return 1;
        }
        int status = 0;
        do
        {
            rc = kickos::apps::wait_dead(victim, &status, 1000u);
        } while (rc == -KOS_ETIMEDOUT);
        // Drops main's hold on a group that is already dead, so the slot goes back here rather
        // than at main's own exit.
        (void)kos_task_slay(victim, 0);
        if (rc != 0)
        {
            break;
        }
#if KICKOS_FS_MODE == 10
        // Read after every trap, so a jump is on the wire before a later trap can end the run.
        // One INT #31 that reaches the clock's wrap handler adds 2^32 counts, minutes at any
        // CMTW1 rate, so a minute bounds the whole loop with room to spare.
        uint64_t const elapsed = kos_clock_now() - clock_before;
        if (elapsed > 60000000000ull and not jumped)
        {
            jumped = true;
            char line[96];
            ksnprintf(line, sizeof(line), "[fs] [clock] JUMPED: %llu ns after trap %u\n",
                      static_cast<unsigned long long>(elapsed), static_cast<unsigned>(run));
            kos::print(line);
        }
#endif
    }
    (void)kos_handle_close(prog);
    kos::print("[fs] survivor ran after the fault\n");
#if KICKOS_FS_MODE == 10
    if (not jumped)
    {
        char line[96];
        ksnprintf(line, sizeof(line), "[fs] [clock] STEADY: %u traps took %llu ns\n",
                  static_cast<unsigned>(runs),
                  static_cast<unsigned long long>(kos_clock_now() - clock_before));
        kos::print(line);
    }
#endif
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
        kos::print("[fs] [lowband] CORRUPTED: the kernel ran below stack_lo on a U-mode sp\n");
    }
    else
    {
        kos::print("[fs] [lowband] INTACT: the kernel wrote nothing below the parked sp\n");
    }
#endif
#if KICKOS_FS_MODE == 3 || KICKOS_FS_MODE == 13
    if (g_fs_kword != FS_KWORD_POISON)
    {
        kos::print("[fs] [kword] CORRUPTED: the kernel stored through the U-mode sp\n");
    }
    else
    {
        kos::print("[fs] [kword] INTACT: nothing was stored through the refused sp\n");
    }
#endif
    if (rc != 0)
    {
        kos::print("[fs] ERROR: join did not report the worker gone\n");
        return 1;
    }
    return 0;
}
