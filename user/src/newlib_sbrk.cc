// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The userspace heap bottom edge: newlib's _sbrk over a bump arena.
//
// Its own translation unit on purpose. A board provisioning no heap defines neither bound
// symbol, so an app pulling malloc fails at link with "undefined reference to
// _kickos_heap_start" instead of returning NULL at runtime. That needs this object pulled
// by a real allocator reference. While it shared a TU with _exit, the fleet-wide
// -Wl,-u,_exit (CMakeLists.txt) force-linked that object into every image, so the strong
// reference was always present and no linker script could withhold it.
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

static void* heap_bump(intptr_t incr)
{
    char* prev = s_brk;
    char* next = s_brk + incr;
    if (next < _kickos_heap_start or next > _kickos_heap_limit)
    {
        return reinterpret_cast<void*>(-1);
    }
    s_brk = next;
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
