// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Fault isolation: a fault taken in unprivileged thread context, in a thread that is
// not already dying, kills that thread and its TASK, and nothing beyond. Every other
// fault panics. The arch handler asks kickos_fault_kill_thread and returns when it says
// yes; the exception return then lands in kickos_thread_fault_exit, which prints and exits.
//
// The task and not the thread because siblings share the address space the faulting thread
// was writing when it died. sched::exit_current draws that line and needs EXIT_FAULTED to
// draw it: a fault sets no cancel_kind, so a contained fault and an ordinary return arrive
// indistinguishable there.

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h> // KOS_EXIT_FAULT, the KOS_NEST_* selectors

#if defined(KICKOS_ENABLE_SELFTEST)
// A kernel .data word an unprivileged thread's trap frame must never be able to reach: the
// faultsurvive `kwrite' arm aims an out-of-bounds sp at it and traps. A backend whose software
// trap prologue stored through the U-mode sp would overwrite this in privileged mode.
extern "C" { volatile uint32_t kickos_trapstack_witness = 0x5A5A5A5Au; }

extern "C" void kickos_trapstack_witness_report(void)
{
    uint32_t const v = kickos_trapstack_witness;
    if (v != 0x5A5A5A5Au)
    {
        ::kickos::kprintf_fault("[trapwitness] CORRUPTED 0x%x\n", static_cast<unsigned>(v));
    }
}

// Nested-trap witness (arch.h). Plain counters and no lock: every writer runs with interrupts
// masked by the trap entry itself, and a torn read of a monotone counter costs an arm one
// count and never a verdict.
//
// rv32imac ONLY: its trap prologue is the one that can place a nested frame on a thread stack.
// Gated because the 16 bytes of kernel .bss are enough to push microbit off the arena cliff.
#if defined(KICKOS_ENABLE_SELFTEST) && defined(__riscv)
namespace
{
    uint32_t g_nest_traps = 0;
    uint32_t g_nest_onstack = 0;
    uint32_t g_nest_minroom = 0;
    bool g_nest_minroom_set = false;
}

extern "C" void kickos_nestwitness_note(uintptr_t frame, uintptr_t lo, uintptr_t hi)
{
    g_nest_traps = g_nest_traps + 1;
    if (lo == 0 or frame < lo or frame >= hi)
    {
        return;
    }
    g_nest_onstack = g_nest_onstack + 1;
    uint32_t const room = static_cast<uint32_t>(frame - lo);
    if (not g_nest_minroom_set or room < g_nest_minroom)
    {
        g_nest_minroom = room;
        g_nest_minroom_set = true;
    }
}

// A plain read and NOT a print: a kprintf here puts kvprintf_route and the console writer
// under the SHUTDOWN syscall and moves the red zone this instrument stands beside.
extern "C" uint32_t kickos_nestwitness_count(int which)
{
    if (which == KOS_NEST_TRAPS)
    {
        return g_nest_traps;
    }
    if (which == KOS_NEST_ONSTACK)
    {
        return g_nest_onstack;
    }
    if (which == KOS_NEST_ROOM)
    {
        if (not g_nest_minroom_set)
        {
            return KOS_NEST_UNSET;
        }
        return g_nest_minroom;
    }
    return KOS_NEST_UNSET;
}
#endif
#endif

namespace
{
    // The window between the redirect and the stub is PREEMPTIBLE (`dying` is not set until
    // exit_current runs), so a second thread's fault can overwrite this before the first stub
    // reads it; kprintf_fault is one such point, waking a console driver that outranks every
    // stdout client. `owner` keeps that honest: a stub that does not own the record prints no
    // fault facts instead of printing another thread's.
    struct FaultRecord
    {
        ::kickos::Thread const* owner;
        char const* status_name;
        uint64_t status;
        uintptr_t pc;
        uintptr_t addr;
        bool addr_valid;
        bool valid;
    };

    constinit ::kickos::InstanceLocal<FaultRecord> g_fault_all = {};

    FaultRecord& fault_record()
    {
        return g_fault_all.get();
    }
}

extern "C" void kickos_fault_record(char const* status_name, uint64_t status,
                                    uintptr_t pc, uintptr_t addr, int addr_valid)
{
    // Runs in the faulting thread's own context, so `current` IS the thread that faulted.
    FaultRecord& r = fault_record();
    r.owner = ::kickos::sched::current();
    r.status_name = status_name;
    r.status = status;
    r.pc = pc;
    r.addr = addr;
    r.addr_valid = (addr_valid != 0);
    r.valid = true;
}

extern "C" bool kickos_fault_frame_trusted(void const* frame, size_t bytes)
{
    ::kickos::Thread* const c = ::kickos::sched::current();
    if (c == nullptr or c == ::kickos::sched::idle())
    {
        return false;
    }
    if (c->stack_base == nullptr or c->stack_size == 0)
    {
        return false;
    }
    uintptr_t const lo = reinterpret_cast<uintptr_t>(c->stack_base);
    uintptr_t const hi = lo + c->stack_size;
    uintptr_t const f = reinterpret_cast<uintptr_t>(frame);
    // Written on the room REMAINING rather than as f + bytes <= hi, so a frame pointer
    // near the top of the address space cannot pass by overflowing the addition.
    return f >= lo and f < hi and bytes <= (hi - f);
}

#if KICKOS_KERNEL_STACKS
// The same guard for a backend whose trap entry has been moved onto the per-thread kernel
// stack: the frame is expected in the RUNNING thread's own block,
// [kernel_sp - KICKOS_KERNEL_STACK_SIZE, kernel_sp), the top being what thread_create seated
// and what the entry loads. kernel_sp 0 is refused: no block was seated, so there is no frame
// of this kind to believe.
extern "C" bool kickos_fault_frame_on_kernel_stack(void const* frame, size_t bytes)
{
    ::kickos::Thread* const c = ::kickos::sched::current();
    if (c == nullptr or c == ::kickos::sched::idle())
    {
        return false;
    }
    uintptr_t const hi = c->ctx.kernel_sp;
    if (hi == 0)
    {
        return false;
    }
    uintptr_t const lo = hi - KICKOS_KERNEL_STACK_SIZE;
    uintptr_t const f = reinterpret_cast<uintptr_t>(frame);
    // Room REMAINING, for the reason stated above.
    return f >= lo and f < hi and bytes <= (hi - f);
}
#endif

// How far below a thread's stack base kickos_fault_below_stack still reads an access as that
// thread running off the bottom of its own stack. ONE RXv3 MPU region page (16 bytes:
// RSPAGEn/REPAGEn hold addr[31:4], RX72M UM sec.17.1.2), the unit the hardware denies in.
//
// It must stay below the distance from a thread's stack base to the nearest legitimate
// cross-domain target beneath it, or a legitimate cross-domain access reads as an overflow;
// below 4 it attributes no overflow at all, the denied push an RXv3 overflow leaves behind
// landing at base - 4.
#ifndef KICKOS_FAULT_STACK_GUARD_BAND
#define KICKOS_FAULT_STACK_GUARD_BAND 16u
#endif

// Where a backend's redirect puts the SP, so the stub runs with the whole stack under it
// rather than at the depth the fault reached. 0 means DO NOT RELOCATE. See arch.h for the
// contract the backends implement against. The answer is the thread's own kernel block where
// one is seated, its user stack otherwise: a sibling sharing the dying thread's domain can
// rewrite the frames of a descent that prints a fault record and then walks cap_teardown and
// the scheduler.
//
// IT IS THE BLOCK'S TOP AND NOT WHERE THE DISPATCH LEFT OFF. Seated at the top, the stub
// DISCARDS any syscall_dispatch frames the block still holds, which is sound because the
// thread is dying and nothing resumes it, and it makes the block requirement the MAX of the
// dispatch class and the exit class rather than their SUM, which no block on any arch holds.
//
// kernel_sp 0 falls back to the user stack: idle, which is outside the pool, and every thread
// on the four armv7m presets where KICKOS_KERNEL_STACKS resolves 0. That fallback is measured
// as the EXIT class of check_trap_redzone.sh against the spawn floor.
extern "C" uintptr_t kickos_fault_stack_top(void)
{
    ::kickos::Thread* const c = ::kickos::sched::current();
    if (c == nullptr or c == ::kickos::sched::idle())
    {
        return 0;
    }
#if KICKOS_KERNEL_STACKS
    if (c->ctx.kernel_sp != 0)
    {
        return c->ctx.kernel_sp;
    }
#endif
    if (c->stack_base == nullptr or c->stack_size == 0)
    {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(c->stack_base) + c->stack_size;
}

extern "C" bool kickos_fault_below_stack(uintptr_t addr)
{
    ::kickos::Thread* const c = ::kickos::sched::current();
    if (c == nullptr or c == ::kickos::sched::idle() or c->stack_base == nullptr)
    {
        return true;
    }
    uintptr_t const base = reinterpret_cast<uintptr_t>(c->stack_base);
    if (addr >= base)
    {
        return false;
    }
    // Subtracted from the BASE rather than added to the address: the other spelling wraps, and
    // a stack based inside the first band of the address space would then admit a wild HIGH
    // address as an overflow.
    if (base < KICKOS_FAULT_STACK_GUARD_BAND)
    {
        return true;
    }
    return addr >= base - KICKOS_FAULT_STACK_GUARD_BAND;
}

extern "C" bool kickos_fault_kill_thread(void* frame)
{
    if (not arch_fault_is_user_thread(frame))
    {
        return false;
    }
    ::kickos::Thread* const c = ::kickos::sched::current();
    // The core's own half of the rule, stated here so it does not rest on every backend's
    // predicate being exact: nothing to kill before the scheduler runs, and neither idle
    // nor a privileged thread may be killed.
    if (c == nullptr or c == ::kickos::sched::idle() or c->privileged)
    {
        return false;
    }
    // Set at the top of exit_current and never cleared, so a fault taken anywhere in the
    // death path (a re-fault on an overflowed stack is the case that matters) reaches the
    // dump instead of looping through the redirect again.
    if (c->dying)
    {
        return false;
    }
    arch_fault_redirect_to_exit(frame);
    return true;
}

extern "C" void kickos_thread_fault_exit(void)
{
    ::kickos::Thread* const c = ::kickos::sched::current();
    char const* who = "?";
    if (c != nullptr and c->name != nullptr)
    {
        who = c->name;
    }
    // kprintf_fault, not kprintf: a published console DROPS the kernel chip path, and this
    // record is the one line naming the dead thread on the boards whose default service list
    // carries a console driver.
    ::kickos::kprintf_fault(KDIAG_F_THREAD_FAULT, who);
    FaultRecord& r = fault_record();
    if (r.valid and r.owner != c)
    {
        // Left valid: the record belongs to a thread whose own stub has not run yet.
        ::kickos::kprintf_fault(KDIAG_F_FAULT_PC_LOST);
    }
    else if (r.valid)
    {
        // Consumed, so a later kill that somehow reaches here without a fresh capture
        // prints nothing rather than this fault's PC.
        r.valid = false;
        if (r.status_name == nullptr)
        {
            // status_name is null on v6-M, whose profile supplies the PC alone.
            ::kickos::kprintf_fault(KDIAG_F_FAULT_PC, reinterpret_cast<void*>(r.pc));
        }
        else
        {
            // uint64_t is `unsigned long` on a 64-bit target and `unsigned long long` on a
            // 32-bit one, so the cast is what keeps ONE format string -Wformat-clean on both.
            ::kickos::kprintf_fault(KDIAG_F_FAULT_PC_STAT, reinterpret_cast<void*>(r.pc),
                                    r.status_name,
                                    static_cast<unsigned long long>(r.status));
        }
        if (r.addr_valid)
        {
            ::kickos::kprintf_fault(KDIAG_F_FAULT_ADDR, reinterpret_cast<void*>(r.addr));
        }
    }
    ::kickos::sched::exit_current(KOS_EXIT_FAULT, ::kickos::sched::EXIT_FAULTED);
}
