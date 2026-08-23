// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Fault isolation: a fault taken in unprivileged thread context, in a thread that is
// not already dying, kills that thread and nothing else. Every other fault panics.
// The arch handler asks kickos_fault_kill_thread and returns when it says yes; the
// exception return then lands in kickos_thread_fault_exit, which prints and exits.

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h> // KOS_EXIT_FAULT, the KOS_NEST_* selectors

#if defined(KICKOS_ENABLE_SELFTEST)
// Trap-stack regression witness. A kernel .data word an unprivileged thread's trap
// frame must never be able to reach: the faultsurvive `kwrite' arm aims an out-of-bounds
// sp at it and traps. A backend whose software trap prologue stored through the U-mode sp
// would overwrite this in privileged mode; the report below runs on the panic path and
// names the corruption, and is silent when the word is intact.
extern "C" { volatile uint32_t kickos_trapstack_witness = 0x5A5A5A5Au; }

extern "C" void kickos_trapstack_witness_report(void)
{
    uint32_t const v = kickos_trapstack_witness;
    if (v != 0x5A5A5A5Au)
    {
        ::kickos::kprintf_fault("[trapwitness] CORRUPTED 0x%x\n", static_cast<unsigned>(v));
    }
}

// Nested-trap witness (arch.h). Written from ISR context, read through a syscall, so plain
// counters and no lock: every writer runs with interrupts masked by the trap entry itself,
// and a torn read of a monotone counter costs an arm one count and never a verdict.
//
// rv32imac ONLY: its trap prologue is the one that can place a nested frame on a thread
// stack, so the arm runs there. Gated because the 16 bytes of kernel .bss are enough to push
// microbit off the arena cliff.
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
// under the SHUTDOWN syscall and moves the red zone this instrument stands beside. The
// caller prints.
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
    // reads it. The print itself is one such point and not merely an async tick: on a
    // published console kprintf_fault wakes the console driver, which outranks every stdout
    // client by provisioning rule. `owner` is what keeps that honest: a stub that does not own
    // the record prints no fault facts instead of printing another thread's. Both threads
    // still die correctly; the cost is that the first one's PC is lost.
    struct FaultRecord
    {
        ::kickos::Thread const* owner;
        char const* status_name;
        uint32_t status;
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

extern "C" void kickos_fault_record(char const* status_name, uint32_t status,
                                    uintptr_t pc, uintptr_t addr, int addr_valid)
{
    // Runs in the faulting thread's own context, so `current` IS the thread this
    // fault belongs to.
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
// stack: the frame is expected in the RUNNING thread's own block, and a frame anywhere else
// was written through a pointer the entry had no business adopting. The block is
// [kernel_sp - KICKOS_KERNEL_STACK_SIZE, kernel_sp), the top being what thread_create seated
// and what the entry loads.
//
// A TCB with kernel_sp 0 is refused: no block was seated for it, so there is no frame of
// this kind to believe. Idle is refused by name as well, as above.
extern "C" bool kickos_fault_frame_on_kernel_stack(void const* frame, size_t bytes)
{
    ::kickos::Thread* const c = ::kickos::sched::current();
    if (c == nullptr or c == ::kickos::sched::idle())
    {
        return false;
    }
    uintptr_t const hi = static_cast<uintptr_t>(c->ctx.kernel_sp);
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
// RSPAGEn/REPAGEn hold addr[31:4], RX72M UM sec.17.1.2), because the page is the unit the
// hardware itself denies in.
//
// The width is a RULING, not a derivation, and its ceiling is a MEASUREMENT: it must stay
// below the distance from a thread's stack base to the nearest legitimate cross-domain target
// beneath it. Widening it past that gap makes a legitimate cross-domain access read as an
// overflow; narrowing it below 4 stops attributing overflows at all, since the denied push an
// RXv3 overflow leaves behind lands at base - 4.
#ifndef KICKOS_FAULT_STACK_GUARD_BAND
#define KICKOS_FAULT_STACK_GUARD_BAND 16u
#endif

// Where a backend's redirect puts the SP, so the stub runs with the whole stack under it
// rather than at the depth the fault reached. 0 means DO NOT RELOCATE, leaving the stub at
// the depth the fault reached rather than aiming it at memory nobody says it may use. See
// arch.h for the contract the backends implement against.
//
// THE THREAD'S OWN KERNEL BLOCK WHERE ONE IS SEATED, and its user stack otherwise. The death
// path is the last place privileged C ran on memory an unprivileged thread can write: the
// address was always kernel-chosen, so this was never the wild-pointer class, but a sibling
// sharing the dying thread's domain could rewrite the frames of a descent that prints a fault
// record and then walks cap_teardown and the scheduler.
//
// IT IS THE BLOCK'S TOP AND NOT WHERE THE DISPATCH LEFT OFF, and that is a requirement rather
// than a convenience. Seated at the top, the stub DISCARDS any syscall_dispatch frames the
// block still holds, which is sound because the thread is dying and nothing resumes it, and
// it is what makes the block requirement the MAX of the dispatch class and the exit class.
// Run nested under a live dispatch frame instead and the requirement becomes their SUM, which
// no block on any arch can hold.
//
// A TCB with kernel_sp 0 falls back to the user stack: idle, which is outside the pool, and
// every thread on the four armv7m presets where KICKOS_KERNEL_STACKS resolves 0. That
// fallback is measured, as the EXIT class of check_trap_redzone.sh against the spawn floor.
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
        return static_cast<uintptr_t>(c->ctx.kernel_sp);
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
            // uint32_t is `unsigned long` on ARM and `unsigned` on the host, so the cast
            // is what keeps ONE format string -Wformat-clean on both.
            ::kickos::kprintf_fault(KDIAG_F_FAULT_PC_STAT, reinterpret_cast<void*>(r.pc),
                                    r.status_name, static_cast<unsigned>(r.status));
        }
        if (r.addr_valid)
        {
            ::kickos::kprintf_fault(KDIAG_F_FAULT_ADDR, reinterpret_cast<void*>(r.addr));
        }
    }
    ::kickos::sched::exit_current(KOS_EXIT_FAULT);
}
