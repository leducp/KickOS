// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/kernel.h>
#include <kickos/instance_local.h>
#include <kickos/sched.h>
#include <kickos/domain.h>
#include <kickos/grant.h> // grant_hits_reserved (backstop assert)
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/libc/string.h>
#include <kickos/task.h>
#include <kickos/reent.h>
#include <kickos/tls.h>

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

#if KICKOS_KERNEL_STACKS
        // Word 0 of a slot's kernel stack, which is its LOW end and so the last word an
        // overflow reaches. 'K','C','A','N'.
        constexpr uint32_t KSTACK_CANARY = 0x4B43414Eu;
        // Every word above the canary, laid once at init. 'K','S','F','L'.
        constexpr uint32_t KSTACK_FILL = 0x4B53464Cu;

        static_assert(KICKOS_KERNEL_STACK_SIZE % sizeof(uint32_t) == 0,
                      "KICKOS_KERNEL_STACK_SIZE must be a whole number of 32-bit words, or "
                      "the canary and the high-water scan would run off the block");
        constexpr size_t KSTACK_WORDS = KICKOS_KERNEL_STACK_SIZE / sizeof(uint32_t);
        static_assert(KSTACK_WORDS >= 2, "KICKOS_KERNEL_STACK_SIZE holds only the canary");
        // Seating an sp needs the arch's stack alignment at the TOP of every slot, which the
        // stride carries; the block's own base carries it through alignas below.
        static_assert(KICKOS_KERNEL_STACK_SIZE % KICKOS_STACK_ALIGN == 0,
                      "KICKOS_KERNEL_STACK_SIZE must be a multiple of KICKOS_STACK_ALIGN, or "
                      "slot i's stack top is misaligned for every odd i");

        // KERNEL .bss AND NOT AN ARENA CARVE, for two reasons. Rule 7 confines every RAM
        // grant to [arch_ram_base(), + arch_ram_size()) and kernel .bss sits below
        // __kickos_ram_start, so no grant a thread can be given reaches these. And
        // arch_ram_alloc snaps size and alignment to what one MPU descriptor can name, which
        // on PMP/NAPOT took one 17408-byte block to 32768 at 32768 alignment; these are never
        // granted, so the arch's stack alignment is all they need.
        //
        // Per instance for the same reason struct Kernel is: the multi-instance sim hosts one
        // kernel per emulated MCU. At one instance the index folds to a literal.
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
    // ARMED ONCE AT INIT AND NEVER RE-ARMED ON SLOT REUSE: re-arming would erase the record
    // of an overflow that already happened, and it would have to run where a slot is handed
    // out, inside the spawn's IrqLock. So both figures below are PER SLOT AND SINCE BOOT
    // rather than per thread, which is also the reading that SIZES a kernel stack.
    void kstack_arm(int index)
    {
        uint32_t* const w = kstack_words(index);
        // THE FILL HAS NO READER IN THE IMAGE, deliberately: the depth a slot reached is
        // read off a memory dump, where the boundary between fill and written words is the
        // measurement. A reader in here would be a second, weaker answer to what
        // check_trap_redzone.sh already measures at build time.
        w[0] = KSTACK_CANARY;
        for (size_t i = 1; i < KSTACK_WORDS; i++)
        {
            w[i] = KSTACK_FILL;
        }
    }

    // False means the low word was overwritten, so the slot's dispatch descended past the
    // whole block. It reports an overflow that has ALREADY happened; it cannot prevent one.
    bool kstack_canary_intact(int index)
    {
        return kstack_words(index)[0] == KSTACK_CANARY;
    }
#endif

    // ONE HOLDER PER DEVICE WINDOW. Matched on RANGES, not on region slots: an encodable
    // window can span several peripheral sub-units or cover part of one, so equal, containing
    // and straddling requests must all refuse while an ADJACENT window stays admissible.
    //
    // Reads each thread's own possession record, never its reachable regions: under a
    // translating backend the window is mapped task-wide and every peer would read as a
    // holder. A thread that has not started yet (INACTIVE) holds nothing, and one that is
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
        //   + [its own stack]                          reachable by its holder
        // Region sizes round to the shape this MPU can describe: a pow2 or a granule
        // multiple, per arch_mpu_region_pow2 (arch_ram_region_size).
        //
        // A REGION BACKEND MAKES THE STACK PRIVATE, AND THAT IS A STRENGTHENING RATHER THAN
        // THE PORTABLE PROMISE. The set is per-thread and reloaded on every switch-in, so a
        // sibling faults on another's stack here and on the sim (where app code/data sit
        // outside the mprotect'd arena and are skipped, but a kernel-default stack is
        // arena-resident). A translating backend maps task-wide instead, and a sibling
        // reaches it. Portable code may rely only on the floor: a thread-scoped grant
        // guarantees access to its HOLDER (docs/design-m6-mmu.md F9).
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
        // The possession record, seated before the region composition that MAPS the window:
        // one is the authority and the other the reach, and only an MPU makes them the same
        // bytes (docs/design-m6-mmu.md F9).
        if (wants_window)
        {
            t->dev_base = reinterpret_cast<uintptr_t>(attr.mmio_base);
            t->dev_size = attr.mmio_size;
        }
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
            // NEVER rounded: rounding would over-grant the neighbouring registers. REACH
            // only: the periph seam is gated on t->dev_base above, never on this region.
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
#if KICKOS_MEMORY_ENFORCED
        for (arch_mpu_region const& r : t->mpu)
        {
            KICKOS_ASSERT(not grant_hits_reserved(r.base, r.size));
        }
#endif

        // THE TLS CARVE, off the LOW end of the thread's own stack, so it costs no MPU
        // descriptor: the region added above already spans it, and raising stack_lo past it
        // keeps the thread's own SP out of its own thread_local storage.
        //
        // IDLE IS THE ONLY THREAD THAT MAY TAKE NONE: its block is a fraction of a stride and
        // its body is arch_idle_wait alone. The exemption is keyed on the idle TCB by
        // IDENTITY and not on the stack's size, because a caller-supplied stack that passed
        // admission would otherwise skip the carve while __aeabi_read_tp went on answering
        // for it.
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
                tls_seat(stack_base);
                ustack = static_cast<unsigned char*>(stack_base) + tls;
                usize = stack_size - tls;
            }
        }
#if KICKOS_KERNEL_STACKS
        // SEATED BEFORE arch_context_init, WHICH READS IT: a backend may build the thread's
        // privileged return state on this block rather than on the stack it is handed, and it
        // cannot do that before the block exists. It is also what lets a trusted entry trust
        // its own stack pointer on the FIRST trap a thread takes.
        //
        // A TCB OUTSIDE THE POOL KEEPS kernel_sp AT 0 and this is its only writer: no
        // arch_context_init touches the field, and the TCB slab hands it whatever it last
        // held, so the unconditional zero has to precede the seating. A stale non-zero value
        // would have a trusted entry load a wild stack pointer. Idle is the only such TCB
        // (kernel().idle_tcb): privileged, and its body is arch_idle_wait alone, so it never
        // enters the syscall path.
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
        // A backend may put an unprivileged thread's SPSR and ELR on this block. With no
        // block that state falls back to the thread's own stack, where a task sibling reaches
        // it. Idle is the only blockless TCB and it is privileged.
        KICKOS_ASSERT(attr.privileged or t->ctx.kernel_sp != 0);
#endif
        arch_context_init(&t->ctx, entry, arg, ustack, usize, attr.privileged);
#if !KICKOS_ARCH_SIM
        // The pool slot indexes the app-side array. A TCB outside the pool takes libc's
        // process-wide state: index_of returns negative for it and the seam answers that
        // with the global rather than with a slot nobody sized. Selection only; the
        // hundreds of bytes kickos_reent_init writes are the caller's, and on the spawn
        // path they are written under the spawn's own lock.
        t->reent = kickos_reent_acquire(kernel().threads.index_of(t));
#endif
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
        // Stamp the trace id into the saved context so the arch switch path can
        // emit it from the physically-swapped contexts (never re-reading sched state).
        arch_trace_stamp_id(&t->ctx, t->id);
#endif
    }

}

// The arch trampoline routes here when a thread's entry function returns.
extern "C" void kickos_thread_return(void)
{
    ::kickos::sched::exit_current(0); // a worker returning normally exits 0
    KICKOS_UNREACHABLE(::kickos::diag::kPastExitCurrent);
}
