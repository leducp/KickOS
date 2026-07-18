// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel bring-up: create the idle + root threads and start the scheduler.
// The root thread calls the application entry (dependency inversion): the app
// owns kickos_app_main(); the kernel boot path calls it after init.

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/domain.h>
#include <kickos/time.h>
#include <kickos/irq.h>
#include <kickos/app.h>
#include <kickos/arch/arch.h>
#include <kickos/config/system.h>
#include <kickos/ktrace.h>

// Buffered-console bring-up (console_tx.cc): binds the TX drain ISR + arms the
// ring once the chip offers a backend. No-op on sim / polled-only chips.
extern "C" void console_buffer_init(void);
// Drain the buffered console before a clean shutdown so a single-shot app's
// trailing output is not stranded in the ring (kpanic/fault already flush).
extern "C" void console_tx_flush_sync(void);

// Non-kernel (app / libstdc++ / newlib / library) global ctors. On a migrated MCU
// the chip linker routes them here, OUT of .init_array (which keeps only the kernel
// ctors that Reset_Handler must run before kmain constructs the instance). We run
// them from root_entry -- in a thread, kernel live -- because a ctor may issue a
// KickOS syscall (kos_clock_now) that needs ktime_init + a current thread. Weak:
// undefined on sim and not-yet-migrated chips -> null -> skipped (there the ctors
// still run via the host runtime / the chip's own full .init_array loop).
extern "C"
{
    extern void (*__kickos_app_init_array_start[])() __attribute__((weak));
    extern void (*__kickos_app_init_array_end[])() __attribute__((weak));
}

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
        // The bootstrap idle/root TCBs + stacks. Still file-static: the remaining
        // instance-scoping residue (invariant #7) -- they move into Kernel with the
        // Later multi-instance work (alongside the sim altstack and the TLS pointer).
        alignas(16) unsigned char g_idle_stack[KICKOS_IDLE_STACK_SIZE];
        alignas(16) unsigned char g_root_stack[KICKOS_ROOT_STACK_SIZE];
        Thread g_idle_tcb;
        Thread g_root_tcb;

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
            char const* rule = "  ==============================================\n";
            kputs("\n");
            kputs(rule);
            kprintf("   KickOS %s  -  microkernel RTOS\n", KICKOS_VERSION);
            kputs(rule);
            kprintf("   board   %s\n", KICKOS_BOARD_NAME);
            kprintf("   arch    %s\n", KICKOS_ARCH_NAME);
            kprintf("   sched   %s\n", sched);
            kprintf("   build   %s %s\n", __DATE__, __TIME__);
            if (kickos_app_build_stamp != nullptr)
            {
                kprintf("   app     %s\n", kickos_app_build_stamp());
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
            // the normal C++ order. Null on sim / unmigrated chips (see the decl).
            if (__kickos_app_init_array_start != nullptr)
            {
                for (void (**fn)() = __kickos_app_init_array_start;
                     fn != __kickos_app_init_array_end; fn++)
                {
                    (*fn)();
                }
            }
            AppArgs const* a = static_cast<AppArgs const*>(arg);
            int status = kickos_app_main(a->argc, a->argv);
            // A returning main is a single-shot app: exit with its status. A
            // daemon-style app never returns here (it parks or loops). Flush the
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
        ktime_init();
        irq_init();          // seed the dispatch table before any driver attaches
        console_buffer_init(); // arm the buffered console TX drain (after irq_init)
        ktrace_init();       // measure probe overhead + emit the opening SESSION (no-op when off)

        ThreadAttr idle_attr;
        idle_attr.name = "idle";
        idle_attr.prio = KICKOS_PRIO_IDLE;
        idle_attr.policy = Policy::FIFO;
        idle_attr.privileged = true;
        thread_create(&g_idle_tcb, idle_entry, nullptr,
                      g_idle_stack, sizeof(g_idle_stack), idle_attr);
        // Idle is created first, so it MUST be trace id 0 (the telemetry decoder
        // keys CPU% off tid 0 == idle). Assert the invariant, not just assume it.
        KICKOS_ASSERT(g_idle_tcb.id == 0);

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
                      g_root_stack, sizeof(g_root_stack), root_attr);

        sched::start(); // returns only if the scheduler ever unwinds to boot
        return 0;
    }
}
