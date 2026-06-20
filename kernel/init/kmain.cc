// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel bring-up: create the idle + root threads and start the scheduler.
// The root thread calls the application entry (dependency inversion): the app
// owns kickos_app_main(); the kernel boot path calls it after init.

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/time.h>
#include <kickos/app.h>
#include <kickos/arch/arch.h>

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
        alignas(16) unsigned char g_idle_stack[64 * 1024];
        alignas(16) unsigned char g_root_stack[64 * 1024];
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
            AppArgs const* a = static_cast<AppArgs const*>(arg);
            int status = kickos_app_main(a->argc, a->argv);
            // A returning main is a single-shot app: exit with its status. A
            // daemon-style app never returns here (it parks or loops).
            arch_shutdown(status);
        }
    }

    int kmain(int argc, char** argv)
    {
        // Local, not static: sched::start() below never unwinds back here (the
        // scheduler exits via arch_shutdown), and root_entry reads these once at
        // entry, so this frame outlives every read.
        AppArgs app_args{argc, argv};

        kbanner();
        sched::init();
        ktime_init();

        ThreadAttr idle_attr;
        idle_attr.name = "idle";
        idle_attr.prio = KICKOS_PRIO_IDLE;
        idle_attr.policy = Policy::FIFO;
        idle_attr.privileged = true;
        thread_create(&g_idle_tcb, idle_entry, nullptr,
                      g_idle_stack, sizeof(g_idle_stack), idle_attr);

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
