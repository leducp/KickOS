// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel bring-up: create the idle + root threads and start the scheduler.
// The root thread calls the application entry (dependency inversion): the app
// owns kickos_app_main(); the kernel boot path calls it after init.

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/cap.h> // cap_seat_authority + CAP_AUTH_ALL (the unprivileged-root seat)
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/grant.h>
#include <kickos/instance.h> // kernel(): root's TCB is an ordinary thread-pool slot
#include <kickos/irqlock.h>
#include <kickos/reent.h>
#include <kickos/time.h>
#include <kickos/irq.h>
#include <kickos/app.h>
#include <kickos/sys/init.h>
#include <kickos/arch/arch.h>
#include <kickos/config/system.h>
#include <kickos/ktrace.h>
#include <kickos/task.h>
#include <kickos/ustack.h>

extern "C"
{
    // Buffered-console bring-up (console_tx.cc): binds the TX drain ISR + arms the
    // ring once the chip offers a backend. No-op on sim / polled-only chips.
    void console_buffer_init(void);
    // Generated per-build by CMake (cmake/build_stamp.cmake) so the banner reflects the
    // image actually linked, not the (stale-on-incremental) __DATE__/__TIME__ of a TU.
    // Local build time (with offset) + the commit (git describe --dirty --always).
    extern char const kickos_build_time[];
    extern char const kickos_build_commit[];
    // Per-app source compile time. Weak REFERENCE: null when no app TU defines it.
    // See app.h and tests/static/weak_allowlist.txt.
    char const* kickos_app_build_stamp(void) __attribute__((weak));

    // Userspace heap window bounds (chip .ld): [_kickos_heap_start, _kickos_heap_limit).
    // Weak: the sim (host heap) and a malloc-free image define neither -> both null ->
    // the banner reports "none". RX prepends one underscore (matches its .ld symbol).
    extern char _kickos_heap_start[] __attribute__((weak));
    extern char _kickos_heap_limit[] __attribute__((weak));
}

#include <stdint.h>

// Identity, injected by the build (see kernel/CMakeLists.txt); fall back so the
// TU still compiles standalone.
#ifndef KICKOS_VERSION
#define KICKOS_VERSION "0.0.0"
#endif
#ifndef KICKOS_BOARD_NAME
#define KICKOS_BOARD_NAME "unknown"
#endif
#ifndef KICKOS_ARCH_NAME
#define KICKOS_ARCH_NAME "unknown"
#endif

namespace kickos
{
    namespace
    {
        // Take one bootstrap thread stack from the user-RAM arena, and assert at boot the
        // two properties an MPU descriptor over it depends on. A .bss array satisfies
        // neither: PMSAv7 and PMP/NAPOT can only name a pow2 block naturally aligned to
        // its size (PMSAv8/SYSMPU/RX name any granule multiple, arch_mpu_region_pow2),
        // and Rule 7 confines a RAM grant to the arena for EVERY caller. Safe here: every
        // arch runs arch_init() before kmain, and the caller has already run domain_init()
        // and grant_reserved_validate().
        void* boot_stack_alloc(size_t size, char const* exhausted_msg)
        {
            void* const p = arch_ram_alloc(size);
            if (p == nullptr)
            {
                // A board that carves no arena, or one too small to hold this stack.
                // Limping on would start a thread on a null stack pointer.
                kpanic(exhausted_msg);
            }
            uintptr_t const base = reinterpret_cast<uintptr_t>(p);
            // Naturally aligned: on a pow2-descriptor arch the alignment IS the region
            // size, so one descriptor names the block and nothing else; on a no-MPU
            // arch the contract is just the 16-byte ABI floor.
            size_t const align = arch_ram_region_align(size);
            KICKOS_ASSERT((base & (align - 1u)) == 0);
            // In-arena across the whole ROUNDED block (the span a descriptor would
            // cover and Rule 7 would admit). Subtract-form bounds cannot wrap.
            size_t const rsz = arch_ram_region_size(size);
            uintptr_t const arena = arch_ram_base();
            size_t const arena_size = arch_ram_size();
            KICKOS_ASSERT(base >= arena);
            KICKOS_ASSERT(rsz <= arena_size);
            KICKOS_ASSERT(base - arena <= arena_size - rsz);
            return p;
        }

        void kbanner()
        {
            char const* sched = "tickless";
#if defined(KICKOS_SCHED_PERIODIC_TICK)
            sched = "periodic tick";
#endif
            char const* mpu = "off";
#if KICKOS_HAVE_MPU
            mpu = "enforce";
#endif
            char const* rule = diag::kBannerRule;
            kputs("\n");
            kputs(rule);
            kprintf(KDIAG_F_BANNER_NAME, KICKOS_VERSION);
            kputs(rule);
            kprintf(KDIAG_F_BANNER_BOARD, KICKOS_BOARD_NAME);
            kprintf(KDIAG_F_BANNER_ARCH, KICKOS_ARCH_NAME);
            kprintf(KDIAG_F_BANNER_MPU, mpu);
            kprintf(KDIAG_F_BANNER_SCHED, sched);
            kprintf(KDIAG_F_BANNER_BUILD, kickos_build_time);
            if (kickos_app_build_stamp != nullptr)
            {
                kprintf(KDIAG_F_BANNER_APP, kickos_app_build_stamp());
            }
            kprintf(KDIAG_F_BANNER_COMMIT, kickos_build_commit);
            uintptr_t const heap_lo = reinterpret_cast<uintptr_t>(_kickos_heap_start);
            uintptr_t const heap_hi = reinterpret_cast<uintptr_t>(_kickos_heap_limit);
            if (heap_hi > heap_lo)
            {
                kprintf(KDIAG_F_BANNER_HEAP,
                        static_cast<unsigned>((heap_hi - heap_lo) / 1024));
            }
            else
            {
                kprintf(KDIAG_F_BANNER_NOHEAP);
            }
#if KICKOS_KERNEL_STACKS
            // Compiled out where the block does not exist: a board that allocates no kernel
            // stacks would be reporting geometry it does not have.
            kprintf(KDIAG_F_BANNER_KSTACK, static_cast<unsigned>(KICKOS_KERNEL_STACK_SIZE),
                    static_cast<unsigned>(KICKOS_THREAD_SLOTS),
                    static_cast<unsigned>(KICKOS_THREAD_SLOTS * KICKOS_KERNEL_STACK_SIZE));
#endif
            kputs("\n");
        }

        void idle_entry(void*)
        {
            while (true)
            {
                arch_idle_wait();
            }
        }
    }

    int kmain(int argc, char** argv)
    {
        // Publish the handoff into app-side storage before anything can read it. Not a
        // frame local: kickos_root_entry reads it as an unprivileged thread on an
        // enforcing board, and the boot stack this frame sits on is outside the arena.
        kickos_init_args.argc = argc;
        kickos_init_args.argv = argv;

        kdiag_led_init(); // early: usable as a fault indicator from here on
        // BEFORE ANY ADDRESS SPACE IS ACTIVATED. kickos_app_build_stamp is APP text, which
        // a translating backend maps privileged-execute-never in a process root; the boot
        // root reached here still grants the privileged fetch.
        kbanner();
        sched::init();
        domain_init(); // build the immortal kernel + default-user domains (arena ready)
        task_init(); // clear the task pool
        grant_reserved_validate(); // Rule 7: reserved set well-formed + arena/app disjoint
        ktime_init();
        irq_init();          // seed the dispatch table before any driver attaches
        console_buffer_init(); // arm the buffered console TX drain (after irq_init)
        ktrace_init();       // measure probe overhead + emit the opening SESSION (no-op when off)

        // Root runs userspace's ctors and then main, and every syscall it makes descends on
        // this stack, so it is bound by the same floor a caller-provided stack is checked
        // against. thread_create takes it with no check of its own, root being created here
        // and not through the spawn syscall, so the relation is asserted rather than tested.
        // Idle is exempt: it is privileged, its body is arch_idle_wait alone, and it never
        // enters the syscall path, so what binds it is the preemption red zone and not this
        // one (see KICKOS_IDLE_STACK_SIZE in Kconfig).
        static_assert(KICKOS_ROOT_STACK_SIZE >= KICKOS_MIN_STACK_SIZE,
                      "KICKOS_ROOT_STACK_SIZE is below the arch's syscall stack floor: root "
                      "makes syscalls, so it would be refused by the trap prologue");

        // Idle (the smaller stack on every board) FIRST: against the bump allocator
        // the small block then sits inside the large block's natural-alignment run-up
        // instead of after it. Worth up to one root-stack's width of arena on every
        // pow2-descriptor board, so KICKOS_BOOT_ARENA_ASSERT models this exact order.
        //
        // WHERE ROOT'S STACK IS FRAMES that model over-reserves rather than describing this,
        // idle being the only arena block left. Both link asserts are floors, so a
        // conservative model refuses nothing that fits; a board tight enough for the
        // difference to matter is a board with no frame pool to move a stack into.
        void* const idle_stack =
            boot_stack_alloc(KICKOS_IDLE_STACK_SIZE, diag::kBootIdleStack);

#if KICKOS_HAVE_ASPACE
        // Its own linker carve, so it takes nothing from the arena idle's stack came out of.
        // FIRST, because root's stack now comes out of it.
        if (not frame_pool_init())
        {
            kpanic(diag::kBootFramePool);
        }
#else
        void* const root_stack =
            boot_stack_alloc(KICKOS_ROOT_STACK_SIZE, diag::kBootRootStack);
        size_t const root_stack_size = KICKOS_ROOT_STACK_SIZE;
#endif

#if KICKOS_KERNEL_STACKS
        // THE ONLY CALLER OF kstack_arm, and every slot is armed here before any of them is
        // handed out. Nothing re-arms on slot reuse, so a slot's high-water and canary read
        // since BOOT rather than per thread; thread.cc holds why that is the wanted reading.
        for (int slot = 0; slot < KICKOS_THREAD_SLOTS; slot++)
        {
            kstack_arm(slot);
        }
#endif

#if !KICKOS_ARCH_SIM
        // Must precede the two thread_create calls below, which acquire out of it.
        reent_seam_read();
#endif

        // Must precede the cap_slab_attach below: this rebuilds the free-chunk list from
        // scratch, so running it afterwards would hand root's run back to the free list.
        cap_slab_init();

        ThreadAttr idle_attr;
        idle_attr.name = "idle";
        idle_attr.prio = KICKOS_PRIO_IDLE;
        idle_attr.policy = Policy::FIFO;
        idle_attr.privileged = true;
        // Idle gets no run at all: an empty directory means capacity 0, so it can neither
        // create, receive nor be delegated a capability.
        idle_attr.cap_run = CapRun{};
        thread_create(&kernel().idle_tcb, idle_entry, nullptr,
                      idle_stack, KICKOS_IDLE_STACK_SIZE, idle_attr);
        // Idle is created first, so it MUST be trace id 0 (the telemetry decoder
        // keys CPU% off tid 0 == idle). Assert the invariant, not just assume it.
        KICKOS_ASSERT(kernel().idle_tcb.id == KICKOS_TID_IDLE);
        // Idle holds no pool slot, so it carries libc's process-wide state and there is
        // nothing to initialise for it.
        sched::add(&kernel().idle_tcb);

        // Root runs at a low priority so a worker's completion post never preempts the
        // orchestrator. That is the only scheduling property the priority buys, and it is
        // NOT a staging guarantee: a spawn does not itself reschedule, but any interrupt
        // between two spawns does, and reschedule() then takes the highest-priority READY
        // thread, which is a prio-10 child and never root at PRIO_MIN+1. A worker can run
        // its entire lifetime before its siblings are spawned. An orchestrator that needs
        // its workers staged must GATE them on a semaphore it posts itself: spawn order is
        // not a barrier.
        ThreadAttr root_attr;
        root_attr.name = "root";
        root_attr.prio = KICKOS_PRIO_MIN + 1;
        root_attr.policy = Policy::FIFO;
        // Privilege is a property of the fabricated first frame (arch_context_init) and
        // of the region set thread_create composes from it, so there is no demotion
        // instant: root is unprivileged from its first instruction. idle above is the
        // only privileged thread in the system.
        root_attr.privileged = false;
#if KICKOS_HAVE_ASPACE
        // ROOT'S TASK IS RESOLVED HERE AND NOT INSIDE thread_create, which is what makes a
        // mapped stack expressible at all: the frames go into root's own address space, and
        // that space does not exist until its domain does. thread_create is then told which
        // task it is, or it would resolve a second one and root would run on frames its own
        // space does not name.
        //
        // AFTER IDLE IS BUILT, and that ordering is not cosmetic: task_for takes no
        // reference, so an uncommitted task slot is still FREE, and idle's own resolve would
        // hand root's slot to the kernel domain, leaving root's stack mapped in a space
        // nothing points at. Neither refusal can fire here (root asks for no grant, and the
        // task and domain pools are all but empty), so there is nothing to unwind.
        int root_derr = 0;
        Task* const root_task = task_for(DOM_CALLER_MEM_AUTH, nullptr, 0, nullptr, &root_derr);
        if (root_task == nullptr)
        {
            kpanic(diag::kBootRootStack);
        }
        root_attr.task = root_task;
        UserStack const root_us =
            ustack_alloc(domain_space(task_domain(root_task)), KICKOS_ROOT_STACK_SIZE);
        if (root_us.base == 0)
        {
            kpanic(diag::kBootRootStack);
        }
        void* const root_stack = reinterpret_cast<void*>(root_us.base);
        size_t const root_stack_size = root_us.bytes;
#endif
        // Root is the only holder of the full summed width; the slab backs exactly this one
        // widening over the child width.
        if (not cap_slab_attach(&root_attr.cap_run, KICKOS_MAX_HANDLES,
                                &root_attr.cap_free_head, &root_attr.cap_width))
        {
            kpanic(diag::kBootRootRun);
        }
        // Root takes an ordinary pool slot, so a handle names it and every generation-guarded
        // mechanism (a reply capability above all) works from root as it does from a child.
        // It leaves spawner_tag at KILL_TAG_NONE and never reaches EXITED, so no kill matches
        // it and no reclaim revisits its slot.
        int root_slot = -1;
        {
            IrqLock lock;
            root_slot = kernel().threads.alloc();
        }
        // Exact, not merely non-negative: ThreadPool::is_root names that one slot and
        // KOS_SYS_EXIT reads it to decide whether an exit ends the system, so an allocation
        // slipped in ahead of root would silently redirect THAT thread's exit. A -1 would
        // name slots[-1], which this rejects too.
        KICKOS_ASSERT(root_slot == ThreadPool::ROOT_INDEX);
        Thread* const root_tcb = &kernel().threads.slots[root_slot];
        // kickos_root_entry and not a local: root is unprivileged from its first
        // instruction, so its text is fetched at EL0, and on a translating board the
        // kernel's half grants EL0 nothing (<kickos/sys/init.h>).
        thread_create(root_tcb, kickos_root_entry, nullptr,
                      root_stack, root_stack_size, root_attr);
        // Root's unkillability rests entirely on this default, which nothing above states.
        KICKOS_ASSERT(root_tcb->spawner_tag == ThreadPool::KILL_TAG_NONE);
        // True would push a KICKOS_ROOT_STACK_SIZE block onto a free list whose one size class
        // is KICKOS_USER_STACK_SIZE.
        KICKOS_ASSERT(not root_tcb->kstack_owned);
        // Root is seated with every authority. Ordering matters twice: after
        // thread_create, which zeroes the cap table and would wipe the seat; before
        // sched::start(), so the seat is in place before root's first instruction.
        // IrqLock is cap_seat_authority's documented precondition.
        {
            IrqLock lock;
            cap_seat_authority(root_tcb, CAP_AUTH_ALL);
        }
        sched::add(root_tcb);

        sched::start(); // returns only if the scheduler ever unwinds to boot
        return 0;
    }
}
