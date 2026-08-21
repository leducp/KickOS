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

// UNSERIALISED read-modify-write. Two preempted threads can both read this s_brk, both pass
// the bounds check and both return the same prev, aliasing the same bytes. The heap window is
// appended to EVERY unprivileged thread, so this is not confined to one task.
//
// A CAS does not link on the ARMv6-M or RX boards: neither has an atomic RMW, and neither
// toolchain ships a libatomic to emulate one, which is where sys/atomic.h's load/store
// surface comes from. The cap table is per-thread with no runtime transfer, so a lock minted
// on first use is unshareable by construction, and static constructors reach _sbrk before an
// init hook could mint one earlier. Interrupt masking is privileged, and unprivileged
// `cpsid i` on ARM is a silent nop. The fix takes the shape of
// arch/common/arch_ram_common.cc, the same bump allocator one privilege level up under
// arch_irq_save, which means a syscall.
//
// Serialising this alone would NOT make multi-threaded malloc safe: newlib's bins stay
// unprotected while __malloc_lock is a no-op (newlib_stubs.cc), so the arena still corrupts.
// Keep such apps single-alloc-thread.
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
