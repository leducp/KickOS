// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/kernel.h>
#include <kickos/instance_local.h>
#include <kickos/sched.h>
#include <kickos/debug.h> // KICKOS_DEBUG_ASSERT
#include <kickos/domain.h>
#include <kickos/grant.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kruntime.h>
#include <kickos/task.h>
#include <kickos/reent.h>
#include <kickos/tls.h>
#include <kickos/ustack.h>

namespace kickos
{
    namespace
    {
        // The first call returns 0, which idle takes. The wrap goes back to 1, never to 0,
        // and skips KICKOS_TID_NONE, so neither sentinel is ever reissued.
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

#if KICKOS_KERNEL_STACKS
        // Word 0 is the slot's low end, the last word an overflow reaches. 'K','C','A','N'.
        constexpr uint32_t KSTACK_CANARY = 0x4B43414Eu;
        // Every word above the canary, laid once at init. 'K','S','F','L'.
        constexpr uint32_t KSTACK_FILL = 0x4B53464Cu;

        static_assert(KICKOS_KERNEL_STACK_SIZE % sizeof(uint32_t) == 0,
                      "KICKOS_KERNEL_STACK_SIZE must be a whole number of 32-bit words, or "
                      "the canary and the high-water scan would run off the block");
        constexpr size_t KSTACK_WORDS = KICKOS_KERNEL_STACK_SIZE / sizeof(uint32_t);
        static_assert(KSTACK_WORDS >= 2, "KICKOS_KERNEL_STACK_SIZE holds only the canary");
        static_assert(KICKOS_KERNEL_STACK_SIZE % KICKOS_STACK_ALIGN == 0,
                      "KICKOS_KERNEL_STACK_SIZE must be a multiple of KICKOS_STACK_ALIGN, or "
                      "slot i's stack top is misaligned for every odd i");

        // Kernel .bss, below __kickos_ram_start, so Rule 7 keeps every RAM grant clear of it
        // and the arch's stack alignment is all these need. Per instance, as struct Kernel is:
        // the multi-instance sim hosts one kernel per emulated MCU.
        struct KStackBlock
        {
            alignas(KICKOS_STACK_ALIGN)
                unsigned char slot[KICKOS_THREAD_SLOTS][KICKOS_KERNEL_STACK_SIZE];
        };
        constinit ::kickos::InstanceLocal<KStackBlock> g_kstacks = {};

        uint32_t* kstack_words(int index)
        {
            KICKOS_ASSERT(index >= 0 and index < KICKOS_THREAD_SLOTS);
            return reinterpret_cast<uint32_t*>(g_kstacks.get().slot[index]);
        }
#endif
    }

#if KICKOS_KERNEL_STACKS
    // Armed once at init and never re-armed on slot reuse: re-arming would erase the record
    // of an overflow that already happened. Both figures below are per slot and since boot.
    void kstack_arm(int index)
    {
        uint32_t* const w = kstack_words(index);
        // The fill has no reader in the image: the depth a slot reached is read off a memory
        // dump, where the boundary between fill and written words is the measurement.
        w[0] = KSTACK_CANARY;
        for (size_t i = 1; i < KSTACK_WORDS; i++)
        {
            w[i] = KSTACK_FILL;
        }
    }

    // False means the low word was overwritten. It reports an overflow that has already
    // happened; it cannot prevent one.
    bool kstack_canary_intact(int index)
    {
        return kstack_words(index)[0] == KSTACK_CANARY;
    }
#endif

    // One holder per device window, matched on RANGES and not on region slots, so equal,
    // containing and straddling requests all refuse while an adjacent window stays
    // admissible. The dying arm is what keeps a respawn issued from the teardown's EPIPE wake
    // from being refused by the very thread whose death freed the device. Check and commit
    // both sit inside thread_create_call's function-scope IrqLock.
    bool dev_window_free(uintptr_t base, size_t size)
    {
        uintptr_t const last = base + size - 1u;
        Kernel& k = kernel();
        for (int i = 0; i < k.threads.next; i++)
        {
            Thread const& t = k.threads.slots[i];
            if (t.state == ThreadState::EXITED or t.state == ThreadState::INACTIVE
                or t.dying or t.dev_size == 0)
            {
                continue;
            }
            if (grant_ranges_overlap(base, last, t.dev_base, t.dev_base + t.dev_size - 1u))
            {
                return false;
            }
        }
        return true;
    }

    void thread_create(Thread* t, void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size, ThreadAttr const& attr)
    {
        kmemset(t, 0, sizeof(*t));
        // Must follow the kmemset, which would otherwise zero the chunk directory AND the
        // free-list head the caller already reserved and threaded.
        t->caps = attr.cap_run;
        t->cap_free_head = attr.cap_free_head;
#if KCAP_RUN_CHUNKS > 1
        t->cap_width = attr.cap_width;
#endif
        t->spawner_tag = attr.spawner_tag;
        t->id = assign_thread_id();
        // NEVER alias attr.name: via thread_create_call it can be a user pointer, and the
        // fault reporter %s-prints t->name, so an unbounded strlen of a bad one would crash
        // the fault path itself.
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

        // A reference on the task is held for the thread's lifetime and released at exit
        // (sched::exit_current).
        t->task = attr.task;
        if (t->task == nullptr)
        {
            // idle/root only, and neither can fail here: no grant is requested and both are
            // among the first task-pool slots, so derr is never set.
            int derr = 0;
            uint32_t caller = DOM_CALLER_MEM_AUTH;
            if (attr.privileged)
            {
                caller |= DOM_CALLER_PRIVILEGED;
            }
            t->task = task_for(caller, attr.mem_base, attr.mem_size, nullptr, &derr);
        }
        task_ref(t->task);
#if KICKOS_KERNEL_CORES > 1
        // AFTER the task is resolved, which is what the default set is read from. idle and
        // root pass 0 and take it.
#if KICKOS_DEBUG
        // The store below is verbatim and nothing downstream re-checks it, so the precondition
        // the spawn boundary satisfies is asserted here rather than left to that one caller.
        uint32_t admitted = 0;
        KICKOS_DEBUG_ASSERT(attr.core_mask == 0
                            or (sched_admit_mask(attr.core_mask, task_core_set(t->task),
                                                 MaskBound::SUBSET, &admitted)
                                    == 0
                                and admitted == attr.core_mask));
#endif
        if (attr.core_mask == 0)
        {
            t->affinity = task_default_cores(t->task);
        }
        else
        {
            t->affinity = attr.core_mask;
        }
#endif

        // An unprivileged thread has no background region, so its set is assembled
        // explicitly: app code and static data, its task's domain regions, its own DEV window
        // and its own stack. Sizes round to what this MPU can describe (arch_mpu_region_pow2).
        //
        // Portable code may rely only on the floor, that a thread-scoped grant reaches its
        // HOLDER; a region backend also makes the stack private, and a translating backend
        // maps it task-wide.
        if (not attr.privileged)
        {
            t->mpu.append_statics();
        }
        bool const wants_stack =
            (not attr.privileged and stack_base != nullptr and stack_size != 0);
        bool const wants_window =
            (not attr.privileged and attr.mmio_base != nullptr and attr.mmio_size != 0);
        // The possession record, seated before the composition that maps the window:
        // authority and reach are the same bytes only on an MPU.
        if (wants_window)
        {
            t->dev_base = reinterpret_cast<uintptr_t>(attr.mmio_base);
            t->dev_size = attr.mmio_size;
        }
        // The whole set MUST fit: a truncated set that drops the thread's own stack would
        // fault it on its own memory.
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
            // The exact window, NEVER rounded: rounding would over-grant the neighbouring
            // registers. Reach only; the periph seam is gated on t->dev_base above.
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

        // Rule 7 backstop: no assembled region may overlap a kernel-reserved block.
#if KICKOS_MEMORY_ENFORCED
        for (arch_mpu_region const& r : t->mpu)
        {
            KICKOS_ASSERT(not grant_hits_reserved(r.base, r.size));
        }
#endif

        // The TLS carve comes off the low end of the thread's own stack, inside the region
        // added above, and raising stack_lo past it keeps the thread's SP out of its own
        // thread_local storage. Idle may take none; the exemption is keyed on the idle TCB by
        // IDENTITY, since keying it on size would let a caller-supplied stack skip the carve
        // while __aeabi_read_tp went on answering for it.
        void* ustack = stack_base;
        size_t usize = stack_size;
        size_t const tls = tls_block_size();
        if (tls != 0)
        {
            bool const admissible =
                tls_stack_admissible(reinterpret_cast<uintptr_t>(stack_base), stack_size);
            KICKOS_ASSERT(admissible or t == &kernel().idle_tcb);
            if (admissible)
            {
                // A mapped stack is named by a virtual address in the CHILD's space, which
                // is not the running one at spawn, so the block is reached through the
                // physical map; VA == PA there. An arena block and a caller-supplied pointer
                // are not pool frames and are directly dereferenceable.
                void* seat = stack_base;
                void* const alias = ustack_kptr(reinterpret_cast<uintptr_t>(stack_base));
                if (alias != nullptr)
                {
                    seat = alias;
                }
                tls_seat(seat);
                ustack = static_cast<unsigned char*>(stack_base) + tls;
                usize = stack_size - tls;
            }
        }
#if KICKOS_KERNEL_STACKS
        // Seated before arch_context_init, which reads it: a backend may build the thread's
        // privileged return state on this block. The unconditional zero must precede the
        // seating, the TCB slab handing kernel_sp whatever it last held and no
        // arch_context_init touching the field, so a stale value would have a trusted entry
        // load a wild stack pointer.
        int const kslot = kernel().threads.index_of(t);
        t->ctx.kernel_sp = 0;
        if (kslot >= 0)
        {
            uintptr_t const top = reinterpret_cast<uintptr_t>(kstack_words(kslot))
                + KICKOS_KERNEL_STACK_SIZE;
            KICKOS_ASSERT((top & (KICKOS_STACK_ALIGN - 1u)) == 0);
            // A narrower field truncates a high-half block silently, and the entry then loads
            // an address in the low half.
            static_assert(sizeof(arch_context::kernel_sp) >= sizeof(uintptr_t),
                          "ctx.kernel_sp is loaded as an address by a trusted entry, so it must "
                          "be pointer-width on every arch that seats a block");
            t->ctx.kernel_sp = top;
        }
        // A backend may put an unprivileged thread's SPSR and ELR on this block; with no
        // block that state falls back to the thread's own stack, where a task sibling reaches
        // it.
        KICKOS_ASSERT(attr.privileged or t->ctx.kernel_sp != 0);
#endif
        arch_context_init(&t->ctx, entry, arg, ustack, usize, attr.privileged);
#if KICKOS_LIBC_REENT
        // Selection only: priming happens at this thread's first switch-in, and the shared
        // state is never primed, being what every prime copies from.
        int const rslot = kernel().threads.index_of(t);
        t->reent = reent_state_for_slot(rslot);
        t->reent_fresh = rslot >= 0;
#endif
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
        arch_trace_stamp_id(&t->ctx, t->id);
#endif
    }

}

// The arch trampoline routes here when a thread's entry function returns.
extern "C" void kickos_thread_return(void)
{
    ::kickos::sched::exit_current(0, ::kickos::sched::EXIT_RETURN);
    KICKOS_UNREACHABLE(::kickos::diag::kPastExitCurrent);
}
