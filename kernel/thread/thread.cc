// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/domain.h>
#include <kickos/grant.h> // grant_hits_reserved (backstop assert)
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
            if (n >= KICKOS_TID_NONE)
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
        t->kstack_owned = attr.kstack_owned;
        t->region_count = 0;
        // slice_deadline_ns is policy-owned: the RR policy arms it on switch-in,
        // before the thread runs; the core carries no slice sentinel.

        // The memory domain: pre-resolved by thread_spawn (so pool exhaustion fails
        // the spawn), else resolved here (idle/root: privileged -> kernel domain,
        // which never fails). A reference is held for the thread's lifetime and
        // released at exit (sched::exit_current).
        t->domain = attr.domain;
        if (t->domain == nullptr)
        {
            // idle/root only (thread_spawn pre-resolves the domain). Both are
            // privileged, so this short-circuits to the kernel domain and the grant
            // predicate never runs; pass caller_privileged=true for that trusted path.
            t->domain = domain_for(attr.privileged, attr.mem_base, attr.mem_size,
                                   attr.mmio_base, attr.mmio_size, true);
        }
        domain_ref(t->domain);

        // MPU region set (reloaded on every switch-in). A privileged (kernel-domain)
        // thread gets the whole arena, and the background region covers its code,
        // kernel data, and stack -- one region suffices. An unprivileged thread has
        // NO background default, so its set is assembled explicitly:
        //   [app code (RX) + app static-data (RW-NX)]  -- so it can run at all
        //   + [domain data region(s)]                  -- what it shares / was granted
        //   + [its own private stack]                  -- a sibling can't scribble it
        // Region sizes round up to the pow2 the MPU can describe (arch_ram_region_size).
        // NOTE: the code/static-data + private-stack regions are enforced by the
        // hardware MPU (PMSA/PMP, where the privileged switch path is exempt via the
        // background region). On the host sim, app code/data still live outside the
        // mprotect'd arena so the sim skips those regions -- but a kernel-default stack
        // is now arena-resident (demand-allocated, not a BSS pool slab), so the sim DOES
        // enforce the private-stack region too: a sibling faults on another's stack.
        size_t nr = 0;
        if (not attr.privileged)
        {
            // App-wide code + static-data regions (linker-defined; empty on no-MPU
            // arches and the sim). These let the unprivileged thread fetch its own
            // instructions and reach its own globals.
            nr += arch_domain_static_regions(&t->regions[nr],
                                             KICKOS_MPU_MAX_REGIONS - nr);
        }
        bool const wants_stack =
            (not attr.privileged and stack_base != nullptr and stack_size != 0);
        // The whole set MUST fit: a truncated set (especially one that drops the
        // thread's OWN stack) would fault the thread on its own memory and, worse,
        // hand it a hardware window snapped to the wrong span. Today's worst case is
        // 5 of 8 (code + appdata + domain data + granted MMIO + stack); a future
        // multi-region domain that overflows is a bug to catch here, not to swallow
        // silently.
        KICKOS_ASSERT(nr + domain_region_count(t->domain)
                          + (wants_stack ? 1u : 0u)
                      <= KICKOS_MPU_MAX_REGIONS);
        if (t->domain != nullptr)
        {
            size_t const dn = domain_region_count(t->domain);
            for (size_t i = 0; i < dn; i++)
            {
                t->regions[nr++] = *domain_region_at(t->domain, i);
            }
        }
        if (wants_stack)
        {
            t->regions[nr].base = reinterpret_cast<uintptr_t>(stack_base);
            t->regions[nr].size = arch_ram_region_size(stack_size);
            t->regions[nr].attr = ARCH_MPU_R | ARCH_MPU_W;
            nr++;
        }
        t->region_count = nr;

        // Rule 7 backstop: no assembled region may overlap a kernel-reserved block.
        // domain_for already refuses an inadmissible grant, so this is defence in
        // depth against a future region source that bypasses that predicate -- it
        // catches the leak at composition, before the thread ever runs. Privileged
        // (kernel-domain) threads carry the whole-arena region, which is reserved-
        // disjoint by grant_reserved_validate at boot.
#if KICKOS_HAVE_MPU
        for (size_t i = 0; i < nr; i++)
        {
            KICKOS_ASSERT(not grant_hits_reserved(t->regions[i].base, t->regions[i].size));
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
    KICKOS_UNREACHABLE("thread continued past exit_current");
}
