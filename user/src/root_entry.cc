// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The root thread's entry, and where a backend translates every unprivileged thread's,
// app-side (see <kickos/sys/init.h> for why). Lives in libkickos_user.a: kmain names the first
// unconditionally.

#include <kickos/sys/abi.h>
#include <kickos/sys/init.h>

#include <kickos/arch/arch.h>

extern "C" void kickos_root_entry(void*)
{
    // Where a backend translates, root entered through kickos_task_start, which ran it.
#if not KICKOS_HAVE_ASPACE
    kickos_image_start_once();
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
    kickos_image_start_once();
    entry(arg);
}
#endif
