// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// An image's startup, app-side (<kickos/sys/init.h>): kickos_image_crt0, a declared task's image's
// start stub where no backend translates, and kickos_image_start_once, its DWARF tables and its
// constructor window, once per image or per space.

#include <kickos/sys/init.h>

#include <kickos/arch/arch.h>
#include <kickos/sys/image.h>

#include <stdint.h>
#include <string.h>

#if not KICKOS_ARCH_SIM
#include <sys/lock.h>
#endif
#if KICKOS_LIBC_REENT and not KICKOS_REENT_PER_THREAD
#include <sys/reent.h>
#endif

// DWARF unwind tables, which an -nostartfiles link leaves unregistered: EHABI and SJLJ unwinders
// register none, the sim's host crt registers its own, and a PE32+ image cannot hold the weak
// reference (system/cxx/exception_report.cc registers there).
#if KICKOS_LINKER_WEAK_UNDEF and not KICKOS_ARCH_SIM and not defined(__ARM_EABI__) \
    and not defined(__USING_SJLJ_EXCEPTIONS__)
#define KICKOS_IMAGE_REGISTERS_FRAMES 1
#else
#define KICKOS_IMAGE_REGISTERS_FRAMES 0
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

#if not KICKOS_ARCH_SIM
    // The image's libc lock, the space's where a backend translates (user/src/newlib_lock.cc).
    extern struct __lock __lock___sfp_recursive_mutex;
#endif

#if KICKOS_IMAGE_REGISTERS_FRAMES
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
#if KICKOS_IMAGE_REGISTERS_FRAMES
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

    enum : uint32_t
    {
        START_PRISTINE = 0,
        START_DONE = 1
    };
    // Per image, and per space where a backend translates: its data starts as the image's,
    // where this is START_PRISTINE.
    uint32_t s_start = START_PRISTINE;
}

#if KICKOS_INIT_IMAGE and not KICKOS_HAVE_ASPACE
// The entry thread is its task's only thread here, and the task the image's only one, so nothing
// reads the data while it is copied. The copy overwrites the libc state word the kernel seats at
// each switch and the arguments kmain published at boot, which are put back after it.
extern "C" void kickos_image_crt0(void)
{
    kos_image_header const& h = kickos_image_header;
#if KICKOS_LIBC_REENT and not KICKOS_REENT_PER_THREAD
    struct _reent* const mine = *static_cast<struct _reent* volatile*>(&_impure_ptr);
#endif
    struct kos_init_args const args = kickos_init_args;
    if (h.data_load != h.data_start)
    {
        memcpy(reinterpret_cast<void*>(h.data_start), reinterpret_cast<void const*>(h.data_load),
               h.data_end - h.data_start);
    }
    if (h.bss_end != h.data_end)
    {
        memset(reinterpret_cast<void*>(h.data_end), 0, h.bss_end - h.data_end);
    }
#if KICKOS_LIBC_REENT and not KICKOS_REENT_PER_THREAD
    *static_cast<struct _reent* volatile*>(&_impure_ptr) = mine;
#endif
    kickos_init_args = args;
}
#endif

// Once per image, and per space where a backend translates, whichever of its threads comes
// first; the others block on the image's libc lock, lending the starter their priority, and
// enter after it. A constructor that waits on another thread of its image therefore waits
// forever. A starter that dies inside leaves the lock to a dead owner, whose next taker traps.
extern "C" void kickos_image_start_once(void)
{
    if (__atomic_load_n(&s_start, __ATOMIC_ACQUIRE) == START_DONE)
    {
        return;
    }
#if not KICKOS_ARCH_SIM
    __retarget_lock_acquire_recursive(&__lock___sfp_recursive_mutex);
#endif
    if (__atomic_load_n(&s_start, __ATOMIC_RELAXED) == START_PRISTINE)
    {
#if KICKOS_HAVE_ASPACE and KICKOS_CLOCK_USER and defined(__x86_64__)
        kickos_clock_seat();
#endif
        image_start();
        __atomic_store_n(&s_start, START_DONE, __ATOMIC_RELEASE);
    }
#if not KICKOS_ARCH_SIM
    __retarget_lock_release_recursive(&__lock___sfp_recursive_mutex);
#endif
}
