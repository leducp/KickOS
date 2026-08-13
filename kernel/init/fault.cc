// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Fault isolation: a fault taken in unprivileged thread context, in a thread that is
// not already dying, kills that thread and nothing else. Every other fault panics.
// The arch handler asks kickos_fault_kill_thread and returns when it says yes; the
// exception return then lands in kickos_thread_fault_exit, which prints and exits.

#include <kickos/arch/arch.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h> // KOS_EXIT_FAULT

namespace
{
    // One record rather than per-Thread fields, because Thread carries no tail padding on any
    // target, so a new field grows every TCB. The window between the redirect and
    // the stub is PREEMPTIBLE (`dying` is not set until exit_current runs), so a second
    // thread's fault can overwrite this before the first stub reads it. The print itself is
    // one such point and not merely an async tick: on a published console kprintf_fault wakes
    // the console driver, which outranks every stdout client by provisioning rule. `owner` is what
    // keeps that honest: a stub that does not own the record prints no fault facts
    // instead of printing another thread's. Both threads still die correctly; the cost
    // is that the first one's PC is lost.
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

    constinit FaultRecord g_fault = {};
}

extern "C" void kickos_fault_record(char const* status_name, uint32_t status,
                                    uintptr_t pc, uintptr_t addr, int addr_valid)
{
    // Runs in the faulting thread's own context, so `current` IS the thread this
    // fault belongs to.
    g_fault.owner = ::kickos::sched::current();
    g_fault.status_name = status_name;
    g_fault.status = status;
    g_fault.pc = pc;
    g_fault.addr = addr;
    g_fault.addr_valid = (addr_valid != 0);
    g_fault.valid = true;
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

extern "C" bool kickos_fault_below_stack(uintptr_t addr)
{
    ::kickos::Thread* const c = ::kickos::sched::current();
    if (c == nullptr or c == ::kickos::sched::idle() or c->stack_base == nullptr)
    {
        return true;
    }
    return addr < reinterpret_cast<uintptr_t>(c->stack_base);
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
    if (g_fault.valid and g_fault.owner != c)
    {
        // Left valid: the record belongs to a thread whose own stub has not run yet.
        ::kickos::kprintf_fault(KDIAG_F_FAULT_PC_LOST);
    }
    else if (g_fault.valid)
    {
        // Consumed, so a later kill that somehow reaches here without a fresh capture
        // prints nothing rather than this fault's PC.
        g_fault.valid = false;
        if (g_fault.status_name == nullptr)
        {
            // v6-M has no fault-status register at all, so there is no word to name.
            ::kickos::kprintf_fault(KDIAG_F_FAULT_PC, reinterpret_cast<void*>(g_fault.pc));
        }
        else
        {
            // uint32_t is `unsigned long` on ARM and `unsigned` on the host, so the cast
            // is what keeps ONE format string -Wformat-clean on both.
            ::kickos::kprintf_fault(KDIAG_F_FAULT_PC_STAT, reinterpret_cast<void*>(g_fault.pc),
                                    g_fault.status_name,
                                    static_cast<unsigned>(g_fault.status));
        }
        if (g_fault.addr_valid)
        {
            ::kickos::kprintf_fault(KDIAG_F_FAULT_ADDR, reinterpret_cast<void*>(g_fault.addr));
        }
    }
    ::kickos::sched::exit_current(KOS_EXIT_FAULT);
}
