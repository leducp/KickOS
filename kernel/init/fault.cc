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
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h> // KOS_EXIT_FAULT
#include <kickos/sys/witness.h>

#if KICKOS_WITNESS_UACCESS
namespace kickos
{
    // syscall_witness.cc: a ring-0 read the witness armed for this thread, here where the frame
    // the kill redirect built is running.
    void witness_uaccess_fault_exit(Thread const* c);
}
#endif

namespace
{
    // The window between the redirect and the stub is PREEMPTIBLE (`dying` is not set until
    // exit_current runs), so a second thread's fault can overwrite this before the first stub
    // reads it. `owner` keeps that honest: a stub that does not own the record prints no fault
    // facts instead of printing another thread's.
    enum class PcKind : uint8_t
    {
        EXACT,
        UNVERIFIED,
        NONE
    };

    struct FaultRecord
    {
        ::kickos::Thread const* owner;
        // Non-null only on a record the owner's slay exit prints.
        char const* banner;
        char const* status_name;
        uint64_t status;
        uintptr_t pc;
        uintptr_t addr;
        // Null where the core latched `addr`.
        char const* addr_unit;
        bool addr_valid;
        bool valid;
        PcKind pc_kind;
        bool svc_dropped;
        // Null on every record but a contained M-profile PSP refusal's.
        char const* why;
        char const* site;
        uint32_t need;
        uint32_t lo;
        uint32_t hi;
    };

    constinit ::kickos::InstanceLocal<FaultRecord[KICKOS_NUM_CORES]> g_fault_all = {};

    FaultRecord& fault_record()
    {
        return g_fault_all.get()[arch_cpu_id()];
    }
}

extern "C" void kickos_fault_record(char const* status_name, uint64_t status,
                                    uintptr_t pc, uintptr_t addr, int addr_valid)
{
    // Runs in the faulting thread's own context, so `current` IS the thread that faulted.
    FaultRecord& r = fault_record();
    r.owner = ::kickos::sched::current();
    r.banner = nullptr;
    r.status_name = status_name;
    r.status = status;
    r.pc = pc;
    r.addr = addr;
    r.addr_unit = nullptr;
    r.addr_valid = (addr_valid != 0);
    r.valid = true;
    r.pc_kind = PcKind::EXACT;
    r.svc_dropped = false;
    r.why = nullptr;
}

extern "C" void kickos_fault_record_unverified_pc(char const* status_name, uint64_t status,
                                                  uintptr_t pc)
{
    kickos_fault_record(status_name, status, pc, 0, 0);
    fault_record().pc_kind = PcKind::UNVERIFIED;
}

extern "C" void kickos_fault_record_svc_dropped(void)
{
    fault_record().svc_dropped = true;
}

extern "C" void kickos_fault_record_no_pc(char const* status_name, uint64_t status,
                                          uintptr_t addr, int addr_valid)
{
    kickos_fault_record(status_name, status, 0, addr, addr_valid);
    fault_record().pc_kind = PcKind::NONE;
}

extern "C" void kickos_fault_record_addr_unit(char const* unit)
{
    fault_record().addr_unit = unit;
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
// kernel_sp 0 falls back to the user stack: every thread on the four armv7m presets where
// KICKOS_KERNEL_STACKS resolves 0. That fallback is measured as the EXIT class of
// check_trap_redzone.sh against the spawn floor.
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

extern "C" struct arch_context* kickos_fault_contain_wild_stack(struct arch_context* offender,
                                                                char const* banner,
                                                                char const* sp_name, uintptr_t sp)
{
    ::kickos::IrqLock lock;
    ::kickos::Thread* const slain = ::kickos::sched::context_owner(offender, lock);
    struct arch_context* const next = kickos_thread_contain_wild_stack(offender, nullptr);
    if (next == nullptr)
    {
        return nullptr;
    }
    slain->wild_stack_contained = 1;
    // `current` is not the offender where a booked switch moved it, so the record names the
    // slain thread itself.
    FaultRecord& r = fault_record();
    r.owner = slain;
    r.banner = banner;
    r.status_name = sp_name;
    r.status = sp;
    r.pc = 0;
    r.addr = 0;
    r.addr_valid = false;
    r.valid = true;
    r.pc_kind = PcKind::NONE;
    r.why = nullptr;
    return next;
}

extern "C" struct arch_context* kickos_fault_contain_wild_psp(struct arch_context* offender,
                                                              char const* banner, char const* why,
                                                              char const* site, uint32_t psp,
                                                              uint32_t need, uint32_t lo,
                                                              uint32_t hi)
{
    struct arch_context* const next = kickos_fault_contain_wild_stack(offender, banner, "PSP", psp);
    if (next == nullptr)
    {
        return nullptr;
    }
    FaultRecord& r = fault_record();
    r.why = why;
    r.site = site;
    r.need = need;
    r.lo = lo;
    r.hi = hi;
    return next;
}

namespace kickos
{
    __attribute__((noinline)) void fault_contained_print(Thread const* t)
    {
        if (t->wild_stack_contained == 0)
        {
            return;
        }
        FaultRecord& r = fault_record();
        if (r.valid and r.owner == t and r.banner != nullptr)
        {
            char const* const banner = r.banner;
            char const* const sp_name = r.status_name;
            unsigned long long const sp = static_cast<unsigned long long>(r.status);
            char const* const why = r.why;
            char const* const site = r.site;
            unsigned const need = r.need;
            unsigned const lo = r.lo;
            unsigned const hi = r.hi;
            r.valid = false;
            kprintf_record("%s", banner);
            kprintf_record(KDIAG_F_FAULT_SP, t->name, sp_name, sp);
            if (why != nullptr)
            {
                kprintf_record(KDIAG_F_FAULT_WILD_PSP, why, site, need, lo, hi);
            }
        }
        else
        {
            kprintf_record(KDIAG_F_THREAD_CONTAINED, t->name);
            kprintf_record(KDIAG_F_FAULT_SP_LOST);
        }
        krecord_end();
    }
}

extern "C" bool kickos_fault_kill_thread(void* frame)
{
    if (not arch_fault_is_user_thread(frame))
    {
        return false;
    }
    ::kickos::Thread* const c = ::kickos::sched::current();
    // The core's own half of the rule, stated here so it does not rest on every backend's
    // predicate being exact: nothing to kill before the scheduler runs, and idle may not be
    // killed.
    if (c == nullptr or c == ::kickos::sched::idle())
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

namespace
{
    // noinline: its locals stay off the frame exit_current descends from.
    __attribute__((noinline)) void fault_record_print(::kickos::Thread const* c)
    {
        char const* who = "?";
        if (c != nullptr)
        {
            who = c->name;
        }
        // Read before the first line: a line can wait for the console, and another thread's
        // fault on this core rewrites the record meanwhile. uint64_t is `unsigned long` on a
        // 64-bit target and `unsigned long long` on a 32-bit one, so the cast is what keeps ONE
        // format string -Wformat-clean on both.
        FaultRecord& r = fault_record();
        bool const lost = r.valid and r.owner != c;
        bool const owned = r.valid and r.owner == c;
        char const* const status_name = r.status_name;
        unsigned long long const status = static_cast<unsigned long long>(r.status);
        void* const pc = reinterpret_cast<void*>(r.pc);
        void* const addr = reinterpret_cast<void*>(r.addr);
        char const* const addr_unit = r.addr_unit;
        bool const addr_valid = r.addr_valid;
        PcKind const pc_kind = r.pc_kind;
        bool const svc_dropped = r.svc_dropped;
        if (owned)
        {
            // Consumed, so a later kill that somehow reaches here without a fresh capture
            // prints nothing rather than this fault's PC.
            r.valid = false;
        }
        // kprintf_record, not kprintf: a published console DROPS the kernel chip path, and this
        // record is the one line naming the dead thread on the boards whose composition carries
        // a console driver.
        ::kickos::kprintf_record(KDIAG_F_THREAD_FAULT, who);
        if (lost)
        {
            // Left valid: the record belongs to a thread whose own stub has not run yet.
            ::kickos::kprintf_record(KDIAG_F_FAULT_PC_LOST);
            return;
        }
        if (not owned)
        {
            return;
        }
        // status_name is null on v6-M, which latches no status.
        if (pc_kind == PcKind::NONE and status_name == nullptr)
        {
            ::kickos::kprintf_record(KDIAG_F_FAULT_NO_PC_BARE);
        }
        else if (pc_kind == PcKind::NONE)
        {
            ::kickos::kprintf_record(KDIAG_F_FAULT_NO_PC, status_name, status);
        }
        else if (pc_kind == PcKind::UNVERIFIED)
        {
            ::kickos::kprintf_record(KDIAG_F_FAULT_PC_UNVERIFIED, pc, status_name, status);
        }
        else if (status_name == nullptr)
        {
            ::kickos::kprintf_record(KDIAG_F_FAULT_PC, pc);
        }
        else
        {
            ::kickos::kprintf_record(KDIAG_F_FAULT_PC_STAT, pc, status_name, status);
        }
        if (addr_valid)
        {
            ::kickos::kprintf_record(KDIAG_F_FAULT_ADDR, addr);
            if (addr_unit != nullptr)
            {
                ::kickos::kprintf_record(KDIAG_F_FAULT_ADDR_UNIT, addr_unit);
            }
        }
        if (svc_dropped)
        {
            ::kickos::kprintf_record(KDIAG_F_FAULT_SVC_DROPPED);
        }
    }
}

extern "C" void kickos_thread_fault_exit(void)
{
    ::kickos::Thread* const c = ::kickos::sched::current();
#if KICKOS_WITNESS_UACCESS
    ::kickos::witness_uaccess_fault_exit(c);
#endif
#if KICKOS_KERNEL_STACKS
    // The stub completes the death on any stack, so this is the only witness that it runs
    // where a domain sibling cannot rewrite its frames.
    KICKOS_ASSERT(c == nullptr or c->ctx.kernel_sp == 0
                  or kickos_fault_frame_on_kernel_stack(__builtin_stack_address(), 0));
#endif
    fault_record_print(c);
    ::kickos::krecord_end();
    ::kickos::sched::exit_current(KOS_EXIT_FAULT, ::kickos::sched::EXIT_FAULTED);
}
