// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The root thread's entry, app-side (see <kickos/sys/init.h> for why). Lives in
// libkickos_user.a: kmain names it unconditionally, so a build selecting its own
// KICKOS_INIT_PROVIDER cannot take the definition away.

#include <kickos/sys/abi.h> // KOS_SYS_SHUTDOWN, KOS_SYS_PANIC
#include <kickos/sys/init.h>

#include <kickos/arch/arch.h>

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
    // DWARF EH unwind tables and libgcc's registrar for them, app-side where the image is split.
    // Weak: null in a freestanding image, and on a target whose unwinder finds the tables another
    // way.
    extern unsigned char __eh_frame_start[];
    void __register_frame(void*) __attribute__((weak));
#endif
}

extern "C" void kickos_root_entry(void*)
{
#if KICKOS_HAVE_ASPACE
    // Before the ctors below, one of which may throw.
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
    int const status = kickos_init_entry(kickos_init_args.argc, kickos_init_args.argv);
    // A returning init is a single-shot system: end it with that status. The trap returning means
    // root was refused.
    arch_syscall(KOS_SYS_SHUTDOWN, static_cast<uintptr_t>(status), 0, 0, 0);
    arch_syscall(KOS_SYS_PANIC, reinterpret_cast<uintptr_t>("root: shutdown refused"), 0, 0,
                 0);
}
