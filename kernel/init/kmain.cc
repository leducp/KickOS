// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/cap.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/grant.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/klink.h>
#include <kickos/reent.h>
#include <kickos/time.h>
#include <kickos/irq.h>
#include <kickos/app.h>
#include <kickos/aspace.h>
#include <kickos/sys/init.h>
#include <kickos/arch/arch.h>
#include <kickos/config/system.h>
#include <kickos/ktrace.h>
#include <kickos/task.h>
#include <kickos/ustack.h>

extern "C"
{
    void console_buffer_init(void);
    // Generated per build by cmake/build_stamp.cmake.
    extern char const kickos_build_time[];
    extern char const kickos_build_commit[];
    // Null when no app TU defines it; the optional reference is declared in
    // tests/static/weak_allowlist.txt.
    extern char const kickos_app_build_time[] KICKOS_LINK_OPTIONAL;

    // Userspace heap window bounds (chip .ld): [_kickos_heap_start, _kickos_heap_limit). Both
    // null where no window is carved, and the banner then reports "none". RX prepends one
    // underscore, matching its .ld symbol.
    extern char _kickos_heap_start[] KICKOS_LINK_OPTIONAL;
    extern char _kickos_heap_limit[] KICKOS_LINK_OPTIONAL;
}

#include <stdint.h>

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
        // Precondition: arch_init(), domain_init() and grant_reserved_validate() have run.
        void* boot_stack_alloc(size_t size, char const* exhausted_msg)
        {
            void* const p = arch_ram_alloc(size);
            if (p == nullptr)
            {
                kpanic(exhausted_msg);
            }
            uintptr_t const base = reinterpret_cast<uintptr_t>(p);
            // On a pow2-descriptor arch the alignment is the region size; elsewhere it is
            // the 16-byte ABI floor.
            size_t const align = arch_ram_region_align(size);
            KICKOS_ASSERT((base & (align - 1u)) == 0);
            // Subtract-form bounds, which cannot wrap, over the whole rounded block.
            size_t const rsz = arch_ram_region_size(size);
            uintptr_t const arena = arch_ram_base();
            size_t const arena_size = arch_ram_size();
            KICKOS_ASSERT(base >= arena);
            KICKOS_ASSERT(rsz <= arena_size);
            KICKOS_ASSERT(base - arena <= arena_size - rsz);
            return p;
        }

        // At namespace scope and volatile. Both name app-half storage, which kernel text
        // cannot materialise on a split image: the linker relaxes the address pair onto the
        // register anchoring the app's small data, which an unprivileged thread writes
        // (tests/static/check_riscv_kernel_gp.sh). A local volatile stops the value folding
        // but not the address being materialised inline.
        char const* const volatile g_app_build_time = kickos_app_build_time;
        struct kos_init_args* const volatile g_init_args_home = &kickos_init_args;

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
            // Read through the link-time word above and, where the image is split, through
            // the kernel's own alias of those bytes: this runs before any address space is
            // activated, so the app's half may have no mapping yet.
            char const* app_stamp = g_app_build_time;
            if (app_stamp != nullptr)
            {
#if KICKOS_HAVE_ASPACE
                char const* const alias
                    = static_cast<char const*>(aspace_image_alias(app_stamp));
                if (alias != nullptr)
                {
                    app_stamp = alias;
                }
#endif
                kprintf(KDIAG_F_BANNER_APP, app_stamp);
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
        // Published into app-side storage before anything can read it: kickos_root_entry
        // reads it unprivileged, and the boot stack this frame sits on is outside the arena.
        // Reached through the kernel's own alias where a translating backend splits the
        // image, the app window not being linked where it loads until the first space is
        // seeded.
        struct kos_init_args* args = g_init_args_home;
#if KICKOS_HAVE_ASPACE
        args = static_cast<struct kos_init_args*>(aspace_image_alias(g_init_args_home));
        if (args == nullptr)
        {
            args = g_init_args_home;
        }
#endif
        args->argc = argc;
        args->argv = argv;

        kdiag_led_init();
        kbanner();
        sched::init();
        domain_init();
        task_init();
        grant_reserved_validate();
        ktime_init();
        irq_init();            // before any driver attaches
        console_buffer_init(); // after irq_init
        ktrace_init();

        static_assert(KICKOS_ROOT_STACK_SIZE >= KICKOS_MIN_STACK_SIZE,
                      "KICKOS_ROOT_STACK_SIZE is below the arch's syscall stack floor: root "
                      "makes syscalls, so it would be refused by the trap prologue");

        // Idle's stack FIRST: KICKOS_BOOT_ARENA_ASSERT models this exact allocation order,
        // so swapping the two makes the link assert describe a boot that no longer happens.
        void* const idle_stack =
            boot_stack_alloc(KICKOS_IDLE_STACK_SIZE, diag::kBootIdleStack);

#if KICKOS_HAVE_ASPACE
        // First: root's stack comes out of this pool.
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
        // The only caller of kstack_arm: nothing re-arms on slot reuse, so a slot's
        // high-water and canary read since boot and not per thread.
        for (int slot = 0; slot < KICKOS_THREAD_SLOTS; slot++)
        {
            kstack_arm(slot);
        }
#endif

#if KICKOS_LIBC_REENT
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
        idle_attr.cap_run = CapRun{};
        thread_create(&kernel().idle_tcb, idle_entry, nullptr,
                      idle_stack, KICKOS_IDLE_STACK_SIZE, idle_attr);
        // Idle is created first, so it MUST be trace id 0: the telemetry decoder keys CPU%
        // off tid 0 == idle.
        KICKOS_ASSERT(kernel().idle_tcb.id == KICKOS_TID_IDLE);
        sched::add(&kernel().idle_tcb);

        // Root runs at a low priority so a worker's completion post never preempts it.
        // Spawn order is not a barrier: any interrupt between two spawns reschedules onto the
        // highest-priority READY thread, so an orchestrator that needs its workers staged
        // MUST gate them on a semaphore it posts itself.
        ThreadAttr root_attr;
        root_attr.name = "root";
        root_attr.prio = KICKOS_PRIO_MIN + 1;
        root_attr.policy = Policy::FIFO;
        // Privilege is a property of the fabricated first frame (arch_context_init), so
        // there is no demotion instant: root is unprivileged from its first instruction.
        root_attr.privileged = false;
#if KICKOS_HAVE_ASPACE
        // Root's task is resolved here and passed to thread_create: the frames go into
        // root's own address space, which does not exist until its domain does, and a second
        // resolve inside thread_create would leave root running on frames its own space does
        // not name. After idle is built, too: task_for takes no reference, so an uncommitted
        // task slot is still free and idle's own resolve would take root's.
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
        if (not cap_slab_attach(&root_attr.cap_run, KICKOS_MAX_HANDLES,
                                &root_attr.cap_free_head, &root_attr.cap_width))
        {
            kpanic(diag::kBootRootRun);
        }
        int root_slot = -1;
        {
            IrqLock lock;
            root_slot = kernel().threads.alloc();
        }
        // Exact, not merely non-negative: ThreadPool::is_root names that one slot and
        // KOS_SYS_EXIT reads it to decide whether an exit ends the system, so an allocation
        // slipped in ahead of root would silently redirect that thread's exit.
        KICKOS_ASSERT(root_slot == ThreadPool::ROOT_INDEX);
        Thread* const root_tcb = &kernel().threads.slots[root_slot];
        // The entry must be app-half text: root is unprivileged from its first instruction,
        // and the kernel's half grants EL0 nothing (<kickos/sys/init.h>).
        thread_create(root_tcb, kickos_root_entry, nullptr,
                      root_stack, root_stack_size, root_attr);
        // Root's unkillability rests entirely on this default, which nothing above states.
        KICKOS_ASSERT(root_tcb->spawner_tag == ThreadPool::KILL_TAG_NONE);
        // True would push a KICKOS_ROOT_STACK_SIZE block onto a free list whose one size class
        // is KICKOS_USER_STACK_SIZE.
        KICKOS_ASSERT(not root_tcb->kstack_owned);
        // Ordering matters twice: after thread_create, which zeroes the cap table and would
        // wipe the seat; before sched::start(), so the seat is in place before root's first
        // instruction. IrqLock is cap_seat_authority's documented precondition.
        {
            IrqLock lock;
            cap_seat_authority(root_tcb, CAP_AUTH_ALL);
        }
        sched::add(root_tcb);

        sched::start(); // returns only if the scheduler ever unwinds to boot
        return 0;
    }
}
