// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The root thread's entry, and where a backend translates every unprivileged thread's,
// app-side (see <kickos/sys/init.h> for why). Lives in libkickos_user.a: kmain names the first
// unconditionally.

#include <kickos/sys/abi.h>
#include <kickos/sys/init.h>

#include <kickos/arch/arch.h>

#if KICKOS_HAVE_ASPACE
#include <kickos/sys/trap.h>

#include <stdint.h>
#include <sys/lock.h>
#endif

// DWARF unwind tables, which an -nostartfiles link leaves unregistered: EHABI and SJLJ unwinders
// register none, the sim's host crt registers its own, and a PE32+ image cannot hold the weak
// reference (system/cxx/exception_report.cc registers there).
#if KICKOS_LINKER_WEAK_UNDEF and not KICKOS_ARCH_SIM and not defined(__ARM_EABI__) \
    and not defined(__USING_SJLJ_EXCEPTIONS__)
#define KICKOS_ROOT_REGISTERS_FRAMES 1
#else
#define KICKOS_ROOT_REGISTERS_FRAMES 0
#endif

extern "C"
{
    // Non-kernel (app / libstdc++ / newlib / library) global ctors. The linker script routes them
    // here, OUT of .init_array, which keeps only the kernel ctors Reset_Handler runs before kmain.
    //
    // STRONG on purpose: every target must STATE its window (the sim states an explicitly EMPTY
    // one, arch/sim/start.cc). Absent is an undefined symbol and the link fails, so a script that
    // forgot to partition .init_array cannot silently run every app ctor privileged.
    extern void (*__kickos_app_init_array_start[])();
    extern void (*__kickos_app_init_array_end[])();

#if KICKOS_HAVE_ASPACE
    // The space's libc lock (user/src/newlib_lock.cc).
    extern struct __lock __lock___sfp_recursive_mutex;
#endif

#if KICKOS_ROOT_REGISTERS_FRAMES
    extern unsigned char __eh_frame_start[];
    // Weak, and NOT one of the bounds include/kickos/klink.h makes strong: it is libgcc's, and
    // an image that never unwinds pulls neither it nor the malloc it calls.
    void __register_frame(void*) __attribute__((weak));
#endif
}

namespace
{
    void image_start()
    {
#if KICKOS_ROOT_REGISTERS_FRAMES
        // HERE AND NOT IN Reset_Handler: the registrar mallocs, and libc finds its state through
        // the thread pointer, which names no block until a thread is switched in. Before the
        // ctors below, one of which may throw.
        if (__register_frame != nullptr)
        {
            __register_frame(__eh_frame_start);
        }
#endif
        for (void (**fn)() = __kickos_app_init_array_start; fn != __kickos_app_init_array_end;
             fn++)
        {
            (*fn)();
        }
    }

#if KICKOS_HAVE_ASPACE
    enum : uint32_t
    {
        START_PRISTINE = 0,
        START_RUNNING = 1,
        START_DONE = 2
    };
    // Per space: every space's data starts as the image's, where this is START_PRISTINE.
    uint32_t s_start = START_PRISTINE;

    // Once per space, whichever of its threads comes first; the others block on the space's
    // libc lock, lending the starter their priority, and enter after it. A constructor that
    // waits on another thread of its space therefore waits forever.
    void image_start_once()
    {
        if (__atomic_load_n(&s_start, __ATOMIC_ACQUIRE) == START_DONE)
        {
            return;
        }
        __retarget_lock_acquire_recursive(&__lock___sfp_recursive_mutex);
        uint32_t const seen = __atomic_load_n(&s_start, __ATOMIC_RELAXED);
        if (seen == START_RUNNING)
        {
            // The starter died inside the walk, holding this lock: the space's data is half
            // constructed, as a heap whose writer died in malloc is torn, and the task ends.
            KOS_TRAP_ILLEGAL();
        }
        if (seen == START_PRISTINE)
        {
            __atomic_store_n(&s_start, START_RUNNING, __ATOMIC_RELAXED);
            image_start();
            __atomic_store_n(&s_start, START_DONE, __ATOMIC_RELEASE);
        }
        __retarget_lock_release_recursive(&__lock___sfp_recursive_mutex);
    }
#endif
}

extern "C" void kickos_root_entry(void*)
{
    // Where a backend translates, root entered through kickos_task_start, which ran it.
#if not KICKOS_HAVE_ASPACE
    image_start();
#endif
    int const status = kickos_init_entry(kickos_init_args.argc, kickos_init_args.argv);
    // A returning init is a single-shot system: end it with that status. The trap returning means
    // root was refused.
    arch_syscall(KOS_SYS_SHUTDOWN, static_cast<uintptr_t>(status), KOS_SHUTDOWN_HALT, 0, 0);
    arch_syscall(KOS_SYS_PANIC, reinterpret_cast<uintptr_t>("root: shutdown refused"), 0, 0,
                 0);
}

#if KICKOS_HAVE_ASPACE
extern "C" void kickos_task_start(void* arg, void (*entry)(void*))
{
    image_start_once();
    entry(arg);
}
#endif
