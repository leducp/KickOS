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
static char* s_brk = _kickos_heap_start;

// Unserialised: two preempted threads can both pass the bounds check and return the same
// prev. The window is granted to every unprivileged thread, so no task boundary confines it.
static void* heap_bump(intptr_t incr)
{
    // The sum is taken on integers: forming s_brk + incr first is undefined as soon as it
    // leaves the object, which is exactly the case the bound below exists to catch.
    uintptr_t const lo = reinterpret_cast<uintptr_t>(_kickos_heap_start);
    uintptr_t const hi = reinterpret_cast<uintptr_t>(_kickos_heap_limit);
    uintptr_t const cur = reinterpret_cast<uintptr_t>(s_brk);
    uintptr_t const next = cur + static_cast<uintptr_t>(incr);
    if (incr > 0 and next < cur)
    {
        return reinterpret_cast<void*>(-1);
    }
    if (incr < 0 and next > cur)
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

#ifdef __RX__
// The RX psABI prefixes every C identifier with a leading underscore at the asm level, so
// the C `_sbrk` above mangles to asm `__sbrk`. Newlib references asm `_sbrk` and would
// otherwise fall through to libnosys sbrk, which pulls `_end` and breaks the app-window
// layout. A C function named `sbrk` mangles to asm `_sbrk` and shares the bump arena.
void* sbrk(intptr_t incr)
{
    return heap_bump(incr);
}
#endif
}
