// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NESTED-TRAP witness (rv32imac): which stack the kernel picks for an interrupt taken
// while it is ALREADY running on a thread's own stack.
//
// rv32imac runs syscall_dispatch privileged, in thread mode, on the CALLER's continuation
// (arch.h arch_syscall contract). An interrupt taken there arrives with mstatus.MPP=M, so
// the trap prologue's U-mode bounds check does not apply, and a prologue that adopted the
// interrupted sp built its frame at whatever depth the dispatch had descended to, then ran
// the ISR below that. The syscall red zone covers the frame plus the dispatch; it never
// covered a second frame plus an ISR under it, so a thread that parks sp at the red-zone
// edge and makes a deep syscall had the kernel write, privileged, below its own stack_lo.
//
// kos_irq_inject, a selftest syscall, raises its line from INSIDE the dispatch with
// interrupts enabled, so the trap fires at that exact instruction on every call: one nested
// M-mode trap each time, with no window to hit. The kernel tallies them (arch.h
// kickos_nestwitness_note).
//
// The worker parks sp at the red-zone EDGE, read from the header the prologue itself reads.
// That is as low as the guard permits the nested frame to land; the tally's second line
// reports the room left under it, to be compared against the interrupt red zone the same
// header derives.
//
// The verdict is the kernel's tally, read back through KOS_SYS_NEST_WITNESS and printed by
// ROOT: traps > 0, the positive control, since nothing provoked proves nothing, and
// onstack == 0.

#include <kickos/arch/rv_trap_stack.h>
#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/emit.h>
#include <kickos/sys/init.h>

#if !defined(__riscv)
#error "trapnest moves sp with RISC-V asm and reads rv32imac's own figures; not for this ISA"
#endif

using kickos::emit;

// KOS_AUTH_IRQ is what the fallback mask lacks, and kos_irq_attach is refused without it,
// which leaves the line MASKED and every inject latched instead of delivered. MEMORY for the
// arena block, SYSTEM because main returns and root_entry shuts down.
KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_IRQ);

namespace
{
    // A line with no hardware source, so unmasking it cannot deliver a real device
    // interrupt into this binding.
#if defined(KICKOS_IRQ_SOFT_ONLY_BASE)
    constexpr int TN_LINE = KICKOS_IRQ_SOFT_ONLY_BASE + 1;
#else
    constexpr int TN_LINE = 7;
#endif

    // Each call is one nested trap, so this is a count of witnesses and not a retry budget.
    constexpr uint32_t TN_INJECTS = 64;

    // Enough real switches for the timer path to be exercised too, short enough that the
    // whole arm stays well inside the runner's timeout.
    constexpr uint32_t TN_SLEEPS = 32;
    constexpr uint64_t TN_SLEEP_NS = 200u * 1000u;

    // Deep-syscall attempts. Each one is a spawn that is REFUSED, on purpose: the refusal
    // happens after the chain has descended, so no thread slot is consumed and the loop can
    // repeat. What has to land in the window is a tick, so this is a budget and not a count.
    constexpr uint32_t TN_SPAWNS = 8192;

    // The tick source, and the reason it is a THREAD and not a quantum: the clock is
    // TICKLESS, so with nothing sleeping no timer is armed and the deep loop below would run
    // to completion without one interrupt landing in it. Higher priority than the worker, so
    // its wake preempts and it gets to re-arm; a lower-priority ticker fires once, becomes
    // ready, never runs again and never sleeps again.
    constexpr uint64_t TN_TICK_NS = 100u * 1000u;

    void ticker(void*)
    {
        while (true)
        {
            kos::sleep_ns(TN_TICK_NS);
        }
    }

    // Power of two and clear of the floor: under enforcement the worker's stack is one PMP
    // region, and PMP NAPOT wants a naturally aligned power of two. Twice the syscall red
    // zone, so the worker still has room for its own frames above the sp it parks at.
    constexpr uint32_t TN_STACK_SIZE = 2048;
    static_assert(TN_STACK_SIZE > KICKOS_RV_TRAP_REDZONE_SYS,
                  "the worker cannot park at the red-zone edge and still have a stack above "
                  "it, so it would fault before reaching the arm");

    // Published by main before the spawn: the worker needs stack_lo and has no syscall that
    // would tell it its own bounds.
    uintptr_t g_tn_stack_lo = 0;

    // Nothing may touch memory between the sp move and the trap, so the whole window is one
    // asm block with every operand already in a register. a0 is the syscall number and a1..a4
    // the arguments (sys/abi.h); the unused ones are ZEROED, as the arch_syscall stub does,
    // in case a dispatch arm ever reads an argument it does not read today.
    void inject_from(uintptr_t sp_target, uint32_t nr, uint32_t line)
    {
        uint32_t saved = 0;
        __asm volatile("mv   %[sv], sp   \n\t"
                       "mv   sp, %[low]  \n\t"
                       "mv   a0, %[nr]   \n\t"
                       "mv   a1, %[ln]   \n\t"
                       "mv   a2, zero    \n\t"
                       "mv   a3, zero    \n\t"
                       "mv   a4, zero    \n\t"
                       "ecall            \n\t"
                       "mv   sp, %[sv]   \n\t"
                       : [sv] "=&r"(saved)
                       : [low] "r"(sp_target), [nr] "r"(nr), [ln] "r"(line)
                       : "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
                         "t0", "t1", "t2", "t3", "t4", "t5", "t6", "memory");
    }

    // The params the deep-syscall attempts hand the kernel. A file-scope object, not a local:
    // the worker's sp is parked far from its own frames when the trap is taken, and the kernel
    // reads this struct out of the caller's memory.
    kos_thread_params g_tn_params = {};
    kos_thread_t g_tn_out = 0;

    // KOS_SYS_THREAD_SPAWN with a stack size under the floor: syscall_thread.cc refuses it,
    // but only after thread_spawn's own frame is live, which is the depth this arm needs. Same
    // one-asm-block discipline as the inject: nothing may touch memory between the sp move
    // and the ecall.
    void spawn_from(uintptr_t sp_target)
    {
        uint32_t saved = 0;
        uint32_t const nr = KOS_SYS_THREAD_SPAWN;
        uintptr_t const p = reinterpret_cast<uintptr_t>(&g_tn_params);
        uintptr_t const out = reinterpret_cast<uintptr_t>(&g_tn_out);
        __asm volatile("mv   %[sv], sp   \n\t"
                       "mv   sp, %[low]  \n\t"
                       "mv   a0, %[nr]   \n\t"
                       "mv   a1, %[pp]   \n\t"
                       "mv   a2, %[oo]   \n\t"
                       "mv   a3, zero    \n\t"
                       "mv   a4, zero    \n\t"
                       "ecall            \n\t"
                       "mv   sp, %[sv]   \n\t"
                       : [sv] "=&r"(saved)
                       : [low] "r"(sp_target), [nr] "r"(nr), [pp] "r"(p), [oo] "r"(out)
                       : "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
                         "t0", "t1", "t2", "t3", "t4", "t5", "t6", "memory");
    }

    void worker(void*)
    {
        uintptr_t const low = g_tn_stack_lo + KICKOS_RV_TRAP_REDZONE_SYS;
        char msg[128];
        ksnprintf(msg, sizeof(msg), "[trapnest] worker parks sp at 0x%x (stack_lo 0x%x + %u)\n",
                  static_cast<unsigned>(low), static_cast<unsigned>(g_tn_stack_lo),
                  static_cast<unsigned>(KICKOS_RV_TRAP_REDZONE_SYS));
        emit(msg);
        for (uint32_t i = 0; i < TN_INJECTS; i++)
        {
            inject_from(low, KOS_SYS_IRQ_INJECT, static_cast<uint32_t>(TN_LINE));
        }
        // Real switches and real timer ticks on top of the injected ones, so the tally also
        // covers a tick taken in the idle thread (privileged, so MPP=M, so its frame used to
        // land on idle's own stack).
        for (uint32_t i = 0; i < TN_SLEEPS; i++)
        {
            kos::sleep_ns(TN_SLEEP_NS);
        }
        // THE DEEPEST SYSCALL an unprivileged thread can reach, from the parked sp. Depth is
        // what decides how far under the frame the nested trap lands, and thread_spawn is the
        // chain rv_trap_stack.h measures the syscall red zone against; the shallower calls
        // above cannot get the frame low enough for the ISR under it to run out of room.
        for (uint32_t i = 0; i < TN_SPAWNS; i++)
        {
            spawn_from(low);
        }
        emit("[trapnest] worker done\n");
    }
}

int main(int, char**)
{
    // One arena block; the worker's stack is its upper half, so stack_lo is a power-of-two
    // boundary the PMP can name. kos_ram_alloc rounds to a describable region size and
    // returns it aligned to that.
    void* const raw = kos_ram_alloc(2u * TN_STACK_SIZE);
    if (raw == nullptr)
    {
        emit("[trapnest] ERROR: the arena cannot spare a caller-owned stack\n");
        return 1;
    }
    uintptr_t const lo = reinterpret_cast<uintptr_t>(raw) + TN_STACK_SIZE;
    if ((lo & (TN_STACK_SIZE - 1u)) != 0)
    {
        emit("[trapnest] ERROR: the arena block is not aligned to the stack size\n");
        return 1;
    }
    g_tn_stack_lo = lo;

    // WHERE the refusal happens is the whole point. A stack under the floor is refused near
    // the TOP of thread_spawn; an INADMISSIBLE memory region is refused in
    // grant_region_admissible, the deep end of the chain rv_trap_stack.h measures the syscall
    // red zone against. So: a kernel-default stack, and a region outside the arena.
    g_tn_params.entry = worker;
    g_tn_params.name = "tndeep";
    g_tn_params.prio = 10;
    g_tn_params.mem_base = reinterpret_cast<void*>(0x1000u);
    g_tn_params.mem_size = 64;

    // MUST be checked: a refused attach leaves the line MASKED, kos_irq_inject then latches
    // the raise instead of delivering it, no trap is taken, and the arm reports traps=0 while
    // looking like it ran.
    kos_cap_t sem = KOS_CAP_NONE;
    if (kos_sem_create(0, &sem) != 0)
    {
        emit("[trapnest] ERROR: sem_create refused\n");
        return 1;
    }
    if (kos_irq_attach(TN_LINE, sem) != 0)
    {
        emit("[trapnest] ERROR: irq_attach refused, so the line stays masked\n");
        return 1;
    }

    kos::thread::Handle const tk = kos::thread::spawn(ticker, nullptr, "tntick", 20);
    if (not tk.valid())
    {
        emit("[trapnest] ERROR: ticker spawn refused\n");
        return 1;
    }
    kos::thread::Handle const w =
        kos::thread::spawn(worker, nullptr, "tnwork", 10, KOS_POLICY_FIFO, 0,
                           /*privileged=*/false, nullptr, 0,
                           reinterpret_cast<void*>(lo), TN_STACK_SIZE);
    if (not w.valid())
    {
        emit("[trapnest] ERROR: worker spawn refused\n");
        return 1;
    }
    if (w.join(KOS_TIMEOUT_NONE) != 0)
    {
        emit("[trapnest] ERROR: join did not report the worker gone\n");
        return 1;
    }

    // Root prints, the kernel only counts: a report written from kernel code would sit on
    // some syscall's path, and on kickos_terminate it put the console's varargs route inside
    // the syscall red zone. traps is the POSITIVE control and is printed with the verdict,
    // never apart from it.
    unsigned const traps = static_cast<unsigned>(kos_nest_witness(KOS_NEST_TRAPS));
    unsigned const onstack = static_cast<unsigned>(kos_nest_witness(KOS_NEST_ONSTACK));
    uint32_t const room = kos_nest_witness(KOS_NEST_ROOM);
    char msg[160];
    ksnprintf(msg, sizeof(msg), "[nestwitness] traps=%u onstack=%u\n", traps, onstack);
    emit(msg);
    if (room != KOS_NEST_UNSET)
    {
        ksnprintf(msg, sizeof(msg),
                  "[nestwitness] closest frame sat %u bytes above stack_lo\n",
                  static_cast<unsigned>(room));
        emit(msg);
    }
    emit("[trapnest] root ran after the worker\n");
    return 0;
}
