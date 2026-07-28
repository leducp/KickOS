// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel bring-up: create the idle + root threads and start the scheduler.
// The root thread calls the application entry (dependency inversion): the app
// owns kickos_app_main(); the kernel boot path calls it after init.

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/domain.h>
#include <kickos/grant.h>
#include <kickos/time.h>
#include <kickos/irq.h>
#include <kickos/app.h>
#include <kickos/sys/init.h>
#include <kickos/arch/arch.h>
#include <kickos/config/system.h>
#include <kickos/ktrace.h>

extern "C"
{
    // Buffered-console bring-up (console_tx.cc): binds the TX drain ISR + arms the
    // ring once the chip offers a backend. No-op on sim / polled-only chips.
    void console_buffer_init(void);
    // Drain the buffered console before a clean shutdown so a single-shot app's
    // trailing output is not stranded in the ring (kpanic/fault already flush).
    void console_tx_flush_sync(void);
    // Generated per-build by CMake (cmake/build_stamp.cmake) so the banner reflects the
    // image actually linked, not the (stale-on-incremental) __DATE__/__TIME__ of a TU.
    // Local build time (with offset) + the commit (git describe --dirty --always).
    extern char const kickos_build_time[];
    extern char const kickos_build_commit[];
    // Per-app source compile time (weak; null when no app defines it). See app.h.
    char const* kickos_app_build_stamp(void) __attribute__((weak));

    // Non-kernel (app / libstdc++ / newlib / library) global ctors. The linker script
    // routes them here, OUT of .init_array (which keeps only the kernel ctors that
    // Reset_Handler must run before kmain constructs the instance). We run them from
    // root_entry -- in a thread, kernel live -- because a ctor may issue a KickOS
    // syscall (kos_clock_now) that needs ktime_init + a current thread.
    //
    // STRONG on purpose: every target must STATE its window. The chip scripts define
    // these bounds around the app-ctor bucket; a hosted target whose own runtime has
    // already run the ctors states an explicitly EMPTY one (arch/sim/start.cc). That
    // makes the two situations distinguishable at LINK time, which is the whole point:
    //   present but empty -> start == end -> the walk below iterates zero times, silent
    //   absent            -> undefined symbol -> the link FAILS, loudly
    // These were once WEAK, which collapsed "absent" into a null the walk skipped. A
    // script that forgot to partition .init_array then ran every app ctor privileged
    // from Reset_Handler *and* skipped the late walk entirely, with no diagnostic --
    // which is exactly how the bluepill-c8 regression stayed silent until d5e7f06.
    extern void (*__kickos_app_init_array_start[])();
    extern void (*__kickos_app_init_array_end[])();

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
        // The bootstrap idle/root TCBs. Still file-static: the remaining
        // instance-scoping residue (invariant #7) -- they move into Kernel with the
        // Later multi-instance work (alongside the sim altstack and the TLS pointer).
        // Their STACKS are deliberately NOT here -- see boot_stack_alloc below.
        Thread g_idle_tcb;
        Thread g_root_tcb;

        // Take one bootstrap thread stack from the user-RAM arena, and prove at boot
        // the two properties an MPU descriptor over it depends on.
        //
        // These stacks used to be .bss arrays, which is a latent isolation bug: the
        // linker puts a .bss object wherever the preceding objects leave the cursor,
        // on the measured frdmk64f link root's 8 KiB stack landed at 0x1fff2be0.
        // A thread's own stack is one of its MPU regions, and PMSAv7/v8 and PMP/NAPOT
        // can only name a power-of-two block whose base is naturally aligned to its
        // size -- so PMSA would have snapped that base down to 0x1fff2000, covering
        // kernel .bss the thread must not reach and missing the top of the stack it
        // must, while NAPOT cannot encode the range at all and fails closed to no
        // stack grant. Only the byte-granular backends (SYSMPU, the RX MPU, the sim's
        // mprotect) would have encoded it as written, which made the failure silently
        // board-dependent. Rule 7 says the same thing from the other side: a RAM grant
        // is confined to the arena for EVERY caller, privileged included
        // (grant_region_admissible), so a stack outside the arena is refusable by the
        // kernel's own rule even where the hardware would have described it.
        //
        // arch_ram_alloc is the fix and the only source of a describable stack: it
        // reserves arch_ram_region_size() bytes naturally aligned to that size, which
        // is exactly one region. It is safe here -- every arch runs arch_init() before
        // kmain, and the caller has already run domain_init() (which reads the same
        // arena bounds) and grant_reserved_validate().
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
            // Naturally aligned. On a pow2-descriptor arch this alignment IS the
            // region size, so one descriptor names the block and nothing else; on a
            // no-MPU arch there is no descriptor and the contract is just the 16-byte
            // ABI floor. Asking the same seam the allocator asked checks what it
            // returned rather than restating the request.
            size_t const align = arch_ram_region_align(size);
            KICKOS_ASSERT((base & (align - 1u)) == 0);
            // In-arena across the whole ROUNDED block -- that is the span a descriptor
            // would cover and the span Rule 7 would admit. Subtract-form bounds, so
            // neither test can wrap.
            size_t const rsz = arch_ram_region_size(size);
            uintptr_t const arena = arch_ram_base();
            size_t const arena_size = arch_ram_size();
            KICKOS_ASSERT(base >= arena);
            KICKOS_ASSERT(rsz <= arena_size);
            KICKOS_ASSERT(base - arena <= arena_size - rsz);
            return p;
        }

        // Host argv forwarded to the app entry (argc=0/argv=nullptr on MCU).
        struct AppArgs
        {
            int argc;
            char** argv;
        };

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
            char const* rule = "  ==============================================\n";
            kputs("\n");
            kputs(rule);
            kprintf("   KickOS %s  -  microkernel RTOS\n", KICKOS_VERSION);
            kputs(rule);
            kprintf("   board   %s\n", KICKOS_BOARD_NAME);
            kprintf("   arch    %s\n", KICKOS_ARCH_NAME);
            kprintf("   mpu     %s\n", mpu);
            kprintf("   sched   %s\n", sched);
            kprintf("   build   %s\n", kickos_build_time);
            if (kickos_app_build_stamp != nullptr)
            {
                kprintf("   app     %s\n", kickos_app_build_stamp());
            }
            kprintf("   commit  %s\n", kickos_build_commit);
            uintptr_t const heap_lo = reinterpret_cast<uintptr_t>(_kickos_heap_start);
            uintptr_t const heap_hi = reinterpret_cast<uintptr_t>(_kickos_heap_limit);
            if (heap_hi > heap_lo)
            {
                kprintf("   heap    %u KiB available\n",
                        static_cast<unsigned>((heap_hi - heap_lo) / 1024));
            }
            else
            {
                kprintf("   heap    none\n");
            }
            kputs("\n");
        }

        void idle_entry(void*)
        {
            while (true)
            {
                arch_idle_wait();
            }
        }

        void root_entry(void* arg)
        {
            // App/library ctors run here (kernel live, in a thread), before main --
            // the normal C++ order. No null guard: the bounds are strong, so an empty
            // window is start == end and this loop simply does not run (see the decl).
            for (void (**fn)() = __kickos_app_init_array_start;
                 fn != __kickos_app_init_array_end; fn++)
            {
                (*fn)();
            }
            AppArgs const* a = static_cast<AppArgs const*>(arg);
            int status = kickos_init_entry(a->argc, a->argv);
            // A returning init is a single-shot system: exit with its status. A
            // persistent init never returns here (it parks or loops). Flush the
            // buffered console first, else trailing output stays stranded in the ring.
            console_tx_flush_sync();
            arch_shutdown(status);
        }
    }

    int kmain(int argc, char** argv)
    {
        // Local, not static: sched::start() below never unwinds back here (the
        // scheduler exits via arch_shutdown), and root_entry reads these once at
        // entry, so this frame outlives every read.
        AppArgs app_args{argc, argv};

        kdiag_led_init(); // early: usable as a fault indicator from here on
        kbanner();
        sched::init();
        domain_init(); // build the immortal kernel + default-user domains (arena ready)
        grant_reserved_validate(); // Rule 7: reserved set well-formed + arena/app disjoint
        ktime_init();
        irq_init();          // seed the dispatch table before any driver attaches
        console_buffer_init(); // arm the buffered console TX drain (after irq_init)
        ktrace_init();       // measure probe overhead + emit the opening SESSION (no-op when off)

        // Both stacks up front, idle (the smaller on every board) FIRST. Against a
        // bump allocator the order decides how much arena the natural-alignment
        // run-ups eat, and taking the small block first lets it sit INSIDE the large
        // block's run-up instead of after it. That is load-bearing, not tidiness: the
        // 16 KiB f302nucleo and 20 KiB bluepill-c8 carve barely 3 KiB of arena, and
        // only this order lets the pair fit at all (it lands flush against the end).
        // The trade is bounded and paid only where there is room: on a board whose
        // arena base is ALREADY root-aligned this strands one root-sized gap, a few
        // KiB out of the hundreds those boards have.
        void* const idle_stack =
            boot_stack_alloc(KICKOS_IDLE_STACK_SIZE, "kmain: no arena for the idle stack");
        void* const root_stack =
            boot_stack_alloc(KICKOS_ROOT_STACK_SIZE, "kmain: no arena for the root stack");

        ThreadAttr idle_attr;
        idle_attr.name = "idle";
        idle_attr.prio = KICKOS_PRIO_IDLE;
        idle_attr.policy = Policy::FIFO;
        idle_attr.privileged = true;
        thread_create(&g_idle_tcb, idle_entry, nullptr,
                      idle_stack, KICKOS_IDLE_STACK_SIZE, idle_attr);
        // Idle is created first, so it MUST be trace id 0 (the telemetry decoder
        // keys CPU% off tid 0 == idle). Assert the invariant, not just assume it.
        KICKOS_ASSERT(g_idle_tcb.id == KICKOS_TID_IDLE);

        // Root runs at a low priority: adding a thread does not itself reschedule,
        // so root still runs first (nothing higher is READY until it spawns them)
        // and does all setup; then, once it blocks, higher-priority workers run,
        // and their completion posts never preempt the low-priority orchestrator.
        ThreadAttr root_attr;
        root_attr.name = "root";
        root_attr.prio = KICKOS_PRIO_MIN + 1;
        root_attr.policy = Policy::FIFO;
        root_attr.privileged = true;
        thread_create(&g_root_tcb, root_entry, &app_args,
                      root_stack, KICKOS_ROOT_STACK_SIZE, root_attr);

        sched::start(); // returns only if the scheduler ever unwinds to boot
        return 0;
    }
}
