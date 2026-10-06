// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The init's stack depth (docs/design-m10-target.md, section 8), on a board whose thread pointer
// is the base of the running thread's stack, the thread-local block being carved off its low end.
// Where SP is masked (M-profile ARM) the thread pointer is __aeabi_read_tp's: SP masked down to
// the stride, which configure holds KICKOS_ROOT_STACK_SIZE to.
// A constructor, which root runs before the init, fills root's stack below itself with a pattern.
// The image links with kos_notify_wait wrapped, so each time root waits the stack is scanned up
// from the thread-local block to the lowest word no longer the pattern, the deepest figure yet is
// printed, and everything below the scanner is filled again, which keeps its own printing out of
// the next scan. Only the walk up to the init's last wait is measured, so the ending of a system
// that ends, the stdout drain and kos_shutdown, is not; the restart witness never ends.

#include <kickos/board_config.h> // KICKOS_ROOT_STACK_SIZE
#include <kickos/sys.h>

#include <stdint.h>
#include <stdio.h>

// A link symbol: its address is the size of the thread-local block.
extern "C" char const __kickos_tls_carve[];

#if defined(__arm__)
extern "C" void* __aeabi_read_tp(void);
#endif

extern "C" int __real_kos_notify_wait(kos_cap_t notify_cap, uint32_t mask, uint32_t timeout_us,
                                      uint32_t* out_bits);

namespace
{
    constexpr uint32_t FILL = 0x5AA5C33Cu;
    // Left unfilled below the filler's own frame.
    constexpr uintptr_t GUARD = 256u;

    uintptr_t root_tp = 0;
    uintptr_t stack_lo = 0;
    uintptr_t deepest = UINTPTR_MAX;

    uintptr_t thread_pointer()
    {
        uintptr_t tp = 0;
#if defined(__aarch64__)
        __asm volatile("mrs %0, tpidr_el0" : "=r"(tp));
#elif defined(__riscv)
        __asm volatile("mv %0, tp" : "=r"(tp));
#elif defined(__arm__) && defined(KICKOS_TLS_FROM_SP) && KICKOS_TLS_FROM_SP
        tp = reinterpret_cast<uintptr_t>(__aeabi_read_tp());
#else
#error "root_depth.cc reads a thread pointer that is the base of its stack"
#endif
        return tp;
    }

    __attribute__((noinline)) void fill()
    {
        uint32_t volatile here = 0;
        uintptr_t const limit = (reinterpret_cast<uintptr_t>(&here) - GUARD) & ~uintptr_t{3u};
        for (uintptr_t p = stack_lo; p < limit; p += 4u)
        {
            *reinterpret_cast<uint32_t volatile*>(p) = FILL;
        }
    }

    __attribute__((noinline)) uintptr_t lowest_written()
    {
        uintptr_t p = stack_lo;
        while (*reinterpret_cast<uint32_t volatile const*>(p) == FILL)
        {
            p += 4u;
        }
        return p;
    }

    __attribute__((constructor)) void arm()
    {
        root_tp = thread_pointer();
        stack_lo = root_tp + reinterpret_cast<uintptr_t>(__kickos_tls_carve);
        fill();
    }
}

extern "C" int __wrap_kos_notify_wait(kos_cap_t notify_cap, uint32_t mask, uint32_t timeout_us,
                                      uint32_t* out_bits)
{
    if (root_tp != 0u and thread_pointer() == root_tp)
    {
        uintptr_t const low = lowest_written();
        if (low < deepest)
        {
            deepest = low;
        }
        printf("root: stack high water %lu of %lu, %lu free above the thread-local block\n",
               static_cast<unsigned long>(root_tp + KICKOS_ROOT_STACK_SIZE - deepest),
               static_cast<unsigned long>(KICKOS_ROOT_STACK_SIZE),
               static_cast<unsigned long>(deepest - stack_lo));
        fill();
    }
    return __real_kos_notify_wait(notify_cap, mask, timeout_us, out_bits);
}
