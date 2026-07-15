// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/libc/string.h>

namespace kickos
{
    namespace
    {
        // Per-Kernel monotonic trace id. First call returns 0 (idle, created first
        // in kmain); thereafter 1,2,...,0xFFFE then wraps to 1 -- 0 is idle-only
        // and 0xFFFF is the "no thread" sentinel, so both are skipped on wrap.
        uint16_t assign_thread_id()
        {
            IrqLock lock;
            Kernel& k = kernel();
            uint16_t id = k.next_tid;
            uint32_t n = static_cast<uint32_t>(k.next_tid) + 1u;
            if (n >= 0xFFFFu)
            {
                n = 1u;
            }
            k.next_tid = static_cast<uint16_t>(n);
            return id;
        }
    }

    void thread_create(Thread* t, void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size, ThreadAttr const& attr)
    {
        memset(t, 0, sizeof(*t));
        t->id = assign_thread_id();
        // Copy the name into a kernel-owned bounded buffer; never alias attr.name -- via
        // thread_spawn it can be a user pointer, and the fault reporter %s-prints t->name
        // (an unbounded strlen / deref of a bad user pointer on the fault path is a crash).
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
        t->region_count = 0;
        // slice_deadline_ns is policy-owned: the RR policy arms it on switch-in,
        // before the thread runs; the core carries no slice sentinel.

        // MPU region set (reloaded on every switch-in). A privileged thread is the
        // kernel domain: the whole user-RAM arena (the background-region analog),
        // which also covers its stack. An unprivileged thread gets its domain data
        // region (if granted) PLUS its own private stack region -- so a sibling
        // cannot scribble its stack, and everything else faults (per-domain
        // isolation). Region sizes round up to the pow2 the MPU can describe
        // (arch_ram_region_size); regions are non-overlapping. NOTE: the private-
        // stack region is enforced by the hardware MPU (Phase 1: PMSA/PMP, where the
        // privileged switch path is exempt via the background region). The host sim
        // leaves it inert -- the default pool stacks live in ungoverned BSS, outside
        // the mprotect'd arena, so grant_region_set fail-closed-skips them; the sim
        // still enforces the arena-resident domain data region.
        size_t nr = 0;
        if (attr.privileged)
        {
            size_t size = arch_ram_size();
            if (size != 0)
            {
                t->regions[nr].base = arch_ram_base();
                t->regions[nr].size = size;
                t->regions[nr].attr = ARCH_MPU_R | ARCH_MPU_W;
                nr++;
            }
        }
        else
        {
            if (attr.mem_base != nullptr and attr.mem_size != 0)
            {
                t->regions[nr].base = reinterpret_cast<uintptr_t>(attr.mem_base);
                t->regions[nr].size = arch_ram_region_size(attr.mem_size);
                t->regions[nr].attr = ARCH_MPU_R | ARCH_MPU_W;
                nr++;
            }
            if (stack_base != nullptr and stack_size != 0)
            {
                t->regions[nr].base = reinterpret_cast<uintptr_t>(stack_base);
                t->regions[nr].size = arch_ram_region_size(stack_size);
                t->regions[nr].attr = ARCH_MPU_R | ARCH_MPU_W;
                nr++;
            }
        }
        t->region_count = nr;

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
    KICKOS_UNREACHABLE("thread continued past exit_current");
}
