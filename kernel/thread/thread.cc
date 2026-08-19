// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/domain.h>
#include <kickos/grant.h> // grant_hits_reserved (backstop assert)
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/libc/string.h>
#include <kickos/task.h>

namespace kickos
{
    namespace
    {
        // Per-Kernel monotonic trace id. The first call returns 0, which idle takes because
        // kmain creates it first; the wrap goes back to 1, never to 0, and skips
        // KICKOS_TID_NONE (0xFFFF), so neither sentinel is ever reissued.
        uint16_t assign_thread_id()
        {
            IrqLock lock;
            Kernel& k = kernel();
            uint16_t id = k.next_tid;
            uint32_t n = static_cast<uint32_t>(k.next_tid) + 1u;
            if (n >= KICKOS_TID_NONE)
            {
                n = 1u;
            }
            k.next_tid = static_cast<uint16_t>(n);
            return id;
        }
    }

    // ONE HOLDER PER DEVICE WINDOW. Matched on RANGES, not on region slots: an encodable
    // window can span several peripheral sub-units or cover part of one, so equal, containing
    // and straddling requests must all refuse while an ADJACENT window stays admissible.
    //
    // Scans THREADS, because a window is a thread's own region and never its task's domain. A
    // thread that has not started yet (INACTIVE) has no regions composed, and one that is
    // EXITED or DYING is not a holder: the dying arm is what keeps a respawn issued from the
    // teardown's EPIPE wake from being refused by the very thread whose death freed the
    // device. Both the check and the commit (thread_create's composition) sit inside
    // thread_spawn's function-scope IrqLock, so they are atomic together.
    bool dev_window_free(uintptr_t base, size_t size)
    {
        uintptr_t const last = base + size - 1u;
        Kernel& k = kernel();
        for (int i = 0; i < k.threads.next; i++)
        {
            Thread const& t = k.threads.slots[i];
            if (t.state == ThreadState::EXITED or t.state == ThreadState::INACTIVE
                or t.dying)
            {
                continue;
            }
            for (arch_mpu_region const& r : t.mpu)
            {
                if ((r.attr & ARCH_MPU_DEV) == 0)
                {
                    continue;
                }
                if (grant_ranges_overlap(base, last, r.base, r.base + r.size - 1u))
                {
                    return false;
                }
            }
        }
        return true;
    }

    void thread_create(Thread* t, void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size, ThreadAttr const& attr)
    {
        memset(t, 0, sizeof(*t));
        // Must follow the memset, which would otherwise zero the chunk directory AND the
        // free-list head the caller already reserved and threaded.
        t->caps = attr.cap_run;
        t->cap_free_head = attr.cap_free_head;
#if KCAP_RUN_CHUNKS > 1
        t->cap_width = attr.cap_width;
#endif
        t->spawner_tag = attr.spawner_tag;
        t->id = assign_thread_id();
        // NEVER alias attr.name: via thread_spawn it can be a user pointer, and the fault
        // reporter %s-prints t->name, so an unbounded strlen of a bad user pointer would
        // crash the fault path itself.
        size_t ni = 0;
        if (attr.name != nullptr)
        {
            for (; ni + 1 < sizeof(t->name_buf) and attr.name[ni] != '\0'; ++ni)
            {
                t->name_buf[ni] = attr.name[ni];
            }
        }
        t->name_buf[ni] = '\0';
        t->name = t->name_buf;
        t->prio = attr.prio;
        t->base_prio = attr.prio;
        t->policy = attr.policy;
        t->quantum_ns = attr.quantum_ns;
        t->privileged = attr.privileged;
        t->state = ThreadState::INACTIVE;
        t->stack_base = stack_base;
        t->stack_size = stack_size;
        t->kstack_owned = attr.kstack_owned;
        t->mpu.clear();
        // slice_deadline_ns is policy-owned: the RR policy arms it on switch-in,
        // before the thread runs; the core carries no slice sentinel.

        // The task, which owns the memory domain: pre-resolved by thread_spawn (so a pool
        // exhaustion fails the spawn), else resolved here (idle/root), where it never
        // fails. A reference is held for the thread's lifetime and released at exit
        // (sched::exit_current).
        t->task = attr.task;
        if (t->task == nullptr)
        {
            // idle/root only (thread_spawn pre-resolves the task). Neither requests a data
            // or MMIO grant, so domain_for short-circuits before the grant predicate and
            // caller_authorized=true is inert, not a waiver; and both are among the first
            // task-pool slots, so the pool arm cannot fire either. No failure arm here, so
            // derr cannot be set.
            int derr = 0;
            t->task = task_for(attr.privileged, attr.mem_base, attr.mem_size, true, &derr);
        }
        task_ref(t->task);

        // MPU region set (reloaded on every switch-in). A privileged (kernel-domain)
        // thread gets the whole arena, and the background region covers its code, kernel
        // data and stack, so one region suffices. An unprivileged thread has NO background
        // default, so its set is assembled explicitly:
        //   [app code (RX) + app static-data (RW-NX)]  so it can run at all
        //   + [domain data region(s)]                  what its TASK shares
        //   + [its own DEV window, if it asked]        one holder per window
        //   + [its own private stack]                  a sibling can't scribble it
        // Region sizes round to the shape this MPU can describe: a pow2 or a granule
        // multiple, per arch_mpu_region_pow2 (arch_ram_region_size).
        // On the host sim, app code/data live outside the mprotect'd arena so the sim skips
        // those regions, but a kernel-default stack is arena-resident, so the sim DOES
        // enforce the private-stack region: a sibling faults on another's stack.
        if (not attr.privileged)
        {
            // App-wide code + static-data regions (linker-defined; empty on no-MPU
            // arches and the sim).
            t->mpu.append_statics();
        }
        bool const wants_stack =
            (not attr.privileged and stack_base != nullptr and stack_size != 0);
        bool const wants_window =
            (not attr.privileged and attr.mmio_base != nullptr and attr.mmio_size != 0);
        // The whole set MUST fit: a truncated set (especially one that drops the thread's
        // OWN stack) would fault the thread on its own memory, or hand it a hardware
        // window snapped to the wrong span. Worst case today is 5 of 8 (code + appdata +
        // task-domain data + own DEV window + stack), and the assert below reads what the
        // appends actually did rather than a second count of what they were going to do.
        bool fitted = true;
        Domain const* const dom = task_domain(t->task);
        if (dom != nullptr)
        {
            size_t const dn = domain_region_count(dom);
            for (size_t i = 0; i < dn; i++)
            {
                arch_mpu_region const* const dr = domain_region_at(dom, i);
                fitted = t->mpu.add(dr->base, dr->size, dr->attr) and fitted;
            }
        }
        if (wants_window)
        {
            // The EXACT window, validated encodable and exclusive at the spawn boundary.
            // NEVER rounded: rounding would over-grant the neighbouring registers. It is
            // this thread's alone, since a task-wide window would hand registers to a peer that
            // never asked (docs/design-task-layer.md section 5.2).
            fitted = t->mpu.add(reinterpret_cast<uintptr_t>(attr.mmio_base), attr.mmio_size,
                                ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV)
                and fitted;
        }
        if (wants_stack)
        {
            fitted = t->mpu.add(reinterpret_cast<uintptr_t>(stack_base),
                                arch_ram_region_size(stack_size), ARCH_MPU_R | ARCH_MPU_W)
                and fitted;
        }
        KICKOS_ASSERT(fitted);

        // Rule 7 backstop: no assembled region may overlap a kernel-reserved block. Catches
        // a region source that bypasses domain_for's admission, at composition, before the
        // thread ever runs. Privileged threads carry the whole-arena region, which
        // grant_reserved_validate proved reserved-disjoint at boot.
#if KICKOS_HAVE_MPU
        for (arch_mpu_region const& r : t->mpu)
        {
            KICKOS_ASSERT(not grant_hits_reserved(r.base, r.size));
        }
#endif

        arch_context_init(&t->ctx, entry, arg, stack_base, stack_size, attr.privileged);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
        // Stamp the trace id into the saved context so the arch switch path can
        // emit it from the physically-swapped contexts (never re-reading sched state).
        arch_trace_stamp_id(&t->ctx, t->id);
#endif
        sched::add(t);
    }

}

// The arch trampoline routes here when a thread's entry function returns.
extern "C" void kickos_thread_return(void)
{
    ::kickos::sched::exit_current(0); // a worker returning normally exits 0
    KICKOS_UNREACHABLE(::kickos::diag::kPastExitCurrent);
}
