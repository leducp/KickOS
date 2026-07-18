// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Newlib-compatible userspace porting layer: the bottom edge that lets the
// toolchain's libstdc++/libsupc++ sit on top of KickOS by routing its OS calls
// (the newlib syscall interface) through KickOS syscalls. NOT compiled
// for the sim (host glibc already provides these symbols); wired in from M1
// when userspace links the ARM toolchain libc.

#include <kickos/sys.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

extern "C"
{

int _write(int fd, char const* buf, int len)
{
    // Pre-driver bootstrap: all libc stdio falls back to the debug console
    // regardless of fd (8l); once a userspace console driver exists, stdout/
    // stderr route to it instead. KickOS has no fd namespace to honor.
    (void)fd;
    return static_cast<int>(kos_kconsole_write(buf, static_cast<size_t>(len)));
}

int _read(int, char*, int)
{
    return 0;
}
int _close(int)
{
    return -1;
}
int _isatty(int)
{
    return 1;
}
int _lseek(int, int, int)
{
    return 0;
}
int _fstat(int, void*)
{
    return 0;
}
int _getpid(void)
{
    return 1;
}
int _kill(int, int)
{
    return -1;
}

// Wall-clock offset over the monotonic kos_clock_now(): unix_ns = now() + offset.
// Default 0 -> wall time reads boot-relative until kos_clock_set_realtime syncs it.
// No RTC/NTP source yet; this is the only writer.
static uint64_t s_wall_offset_ns = 0;

void kos_clock_set_realtime(uint64_t unix_ns)
{
    s_wall_offset_ns = unix_ns - kos_clock_now();
}

// Backs newlib's gettimeofday() (and so std::chrono::system_clock::now()).
// steady_clock deliberately does NOT route through here: this toolchain's
// libstdc++ implements steady_clock::now() and system_clock::now() identically,
// both calling gettimeofday -- so a monotonic path must bypass libc and call
// kos_clock_now() directly (see kickcat's OS/KickOS/Time.cc::now()).
int _gettimeofday(struct timeval* tv, void*)
{
    uint64_t wall_ns = kos_clock_now() + s_wall_offset_ns;
    tv->tv_sec = static_cast<time_t>(wall_ns / 1000000000ull);
    tv->tv_usec = static_cast<suseconds_t>((wall_ns % 1000000000ull) / 1000ull);
    return 0;
}

void _exit(int code)
{
    kos_exit(code);
    while (true)
    {
    }
}

// Bump allocator over a fixed userspace heap arena.
static char s_heap[64 * 1024];
static char* s_brk = s_heap;

void* _sbrk(intptr_t incr)
{
    char* prev = s_brk;
    char* next = s_brk + incr;
    if (next < s_heap or next > s_heap + sizeof(s_heap))
    {
        return reinterpret_cast<void*>(-1);
    }
    s_brk = next;
    return prev;
}

// Newlib's malloc brackets every arena mutation with __malloc_lock/__malloc_unlock.
// The pinned vendor toolchains are all built single-thread (--disable-threads), so
// no reentrancy guard is needed for the single-threaded full-C++ opt-in (see the
// design's "single-thread" caveat). Provide no-op weak stubs so a full-C++ app that
// heap-allocates (operator new -> malloc) links; weak so a future thread-safe libc
// port can override, and unreferenced (freestanding app) so it costs nothing.
// FOOTGUN: a full-C++ app that spawns threads and heap-allocates from more than one
// gets NO reentrancy guard here -- concurrent malloc silently corrupts the arena.
// Real thread-safe stubs (an IrqLock or per-thread arena) are a prerequisite of the
// multi-threaded-full-C++ milestone; until then keep such apps single-alloc-thread.
__attribute__((weak)) void __malloc_lock(void*)
{
}
__attribute__((weak)) void __malloc_unlock(void*)
{
}
}
