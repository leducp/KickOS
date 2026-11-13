// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The userspace heap bottom edge: newlib's _sbrk over a bump arena.
//
// Its own translation unit on purpose. A board provisioning no heap defines neither
// bound symbol, so an app pulling malloc fails at link ("undefined reference to
// _kickos_heap_start") instead of returning NULL at runtime. That needs this object
// pulled only by a real allocator reference; the fleet-wide -Wl,-u,_exit force-links
// newlib_stubs.o into every image, so _sbrk cannot live there.
//
// Do not add anything here that an image might want without a heap.

#include <kickos/sys.h>

#include <newlib.h>
#include <stddef.h>
#include <stdint.h>

extern "C"
{

// The bounds are linker symbols, not a static array: on an MPU chip the heap is the unused
// pad of the granted .appdata window; on a non-MPU chip it is an explicit .userheap section
// (arch/*/chip/*.ld). RX prepends one underscore, so the C `_kickos_heap_start` here
// resolves to the linker symbol `__kickos_heap_start` the RX .ld defines.
extern char _kickos_heap_start[];
extern char _kickos_heap_limit[];
// Undeclared by a freestanding <stdlib.h>.
void* malloc(size_t);
void free(void*);
void kickos_heap_seat(void);
void kickos_reent_heap_used(void);
static char* s_brk = _kickos_heap_start;

// Unserialised: newlib calls it under malloc's lock, and a direct sbrk() outside that lock can
// return the prev another caller got. The window is granted to every unprivileged thread, so no
// task boundary confines it.
static void* heap_bump(intptr_t incr)
{
    // The sum is taken on integers: forming s_brk + incr first is undefined as soon as it
    // leaves the object, which is exactly the case the bound below exists to catch.
    uintptr_t const lo = reinterpret_cast<uintptr_t>(_kickos_heap_start);
    uintptr_t const hi = reinterpret_cast<uintptr_t>(_kickos_heap_limit);
    uintptr_t const cur = reinterpret_cast<uintptr_t>(s_brk);
    uintptr_t const next = cur + static_cast<uintptr_t>(incr);
    // Grow-only: a break moved back would leave newlib's full malloc a top it regrows only in
    // whole pages (kickos_heap_seat).
    if (incr < 0)
    {
        return reinterpret_cast<void*>(-1);
    }
    if (next < cur)
    {
        return reinterpret_cast<void*>(-1);
    }
    if (next < lo or next > hi)
    {
        return reinterpret_cast<void*>(-1);
    }
    char* const prev = s_brk;
    s_brk = reinterpret_cast<char*>(next);
    return prev;
}

void* _sbrk(intptr_t incr)
{
    return heap_bump(incr);
}

#if not defined(_NANO_MALLOC)
static bool s_seated = false;
#endif

// newlib's full malloc takes its first block from _sbrk at the size it needs and every later one
// in whole 4 KiB pages ending on a 4 KiB address, so once it holds a block it cannot reach the end
// of a window that ends off such an address. Its first block here is the whole window, freed at
// once: the largest request that fits. Run under the malloc lock (newlib_lock.cc), once.
void kickos_heap_seat(void)
{
    // Both profiles: until this runs, an exiting thread frees no libc scratch (newlib_reent.cc).
    kickos_reent_heap_used();
#if not defined(_NANO_MALLOC)
    if (s_seated)
    {
        return;
    }
    s_seated = true;
    constexpr size_t STEP = 8;
    // More than the allocator's own header and alignment take from a request.
    constexpr size_t SLACK = 128;
    size_t const room = static_cast<size_t>(_kickos_heap_limit - s_brk);
    size_t n = room;
    while (n >= STEP and n + SLACK >= room)
    {
        void* const block = malloc(n);
        if (block != nullptr)
        {
            free(block);
            return;
        }
        n -= STEP;
    }
#endif
}

#if defined(__RX__) || defined(__x86_64__)
// The RX psABI prefixes every C identifier with a leading underscore at the asm level, so
// the C `_sbrk` above mangles to asm `__sbrk`. Newlib references asm `_sbrk` and would
// otherwise fall through to libnosys sbrk, which pulls `_end` and breaks the app-window
// layout. A C function named `sbrk` mangles to asm `_sbrk` and shares the bump arena.
// x86_64-elf's newlib references `sbrk` itself, being built with MISSING_SYSCALL_NAMES.
void* sbrk(intptr_t incr)
{
    return heap_bump(incr);
}
#endif
}
