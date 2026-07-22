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

// Stdout probe state, one per process lifetime: 0 unprobed, 1 seated (send to the
// console endpoint at cap index 0), -1 dead/not-seated (STICKY -- fall back to the debug
// console forever, one wasted syscall total). A re-published target is picked up only by
// a freshly spawned process (the design's accepted D8 limitation).
static int g_stdout_probe = 0;

int _write(int fd, char const* buf, int len)
{
    // KickOS has no fd namespace: all libc stdio (any fd) goes to the console. If a
    // userspace driver has taken stdout (cap index 0 seated), route there via kos_send;
    // otherwise fall back to the debug console (kconsole_write). See the handover design (D5).
    (void)fd;
    if (len <= 0)
    {
        return 0;
    }
    size_t const total = static_cast<size_t>(len);
    if (g_stdout_probe == -1)
    {
        return static_cast<int>(kos_kconsole_write(buf, total)); // sticky: never re-probe
    }
    size_t sent = 0;
    while (sent < total)
    {
        size_t chunk = total - sent;
        if (chunk > KOS_EP_MSG_MAX)
        {
            chunk = KOS_EP_MSG_MAX;
        }
        long const r = kos_send(0, buf + sent, chunk); // index 0 == the stdout endpoint cap
        if (r < 0)
        {
            // Pre-handover (index 0 empty) or the driver died (EPIPE): pin the probe to -1
            // and fall back on the REMAINDER only -- resending the whole buffer would
            // duplicate on the debug console the chunks already delivered to the driver.
            // Return the FULL len (not the remainder): the first `sent` bytes were already
            // accepted via IPC, so reporting a short write would make newlib retry and
            // re-send them. Mirror the success path's `return len`.
            g_stdout_probe = -1;
            kos_kconsole_write(buf + sent, total - sent);
            return len;
        }
        g_stdout_probe = 1;
        sent += static_cast<size_t>(r);
    }
    return len;
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

#ifdef __riscv
// Newlib's getentropy() bottom edge, needed by the RISC-V full-C++ link: KEEPing
// .eh_frame (DWARF EH) retains the libc arc4random/getentropy FDEs, which pin the
// getentropy dependency chain against --gc-sections, so this symbol must resolve to
// link. NOT a cryptographic source -- KickOS exposes no HW RNG yet; seeded weakly off
// the monotonic clock so the buffer is merely non-constant. Callers needing real
// entropy must wait for an RNG driver. Guarded for RISC-V (ARM uses EHABI .ARM.exidx,
// keeps no .eh_frame, and never pulls this chain).
int _getentropy(void* buf, size_t len)
{
    uint64_t x = kos_clock_now();
    unsigned char* p = static_cast<unsigned char*>(buf);
    for (size_t i = 0; i < len; i++)
    {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        p[i] = static_cast<unsigned char>(x >> 56);
    }
    return 0;
}
#endif

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

// Bump allocator over a fixed userspace heap arena. Size is provisioned: full-C++
// under MPU enforcement routes s_heap into the granted .appdata window, which is a
// pow2 region -- a small-RAM part sets a smaller KICKOS_HEAP_SIZE so the window fits
// (default 64K suits a 256K-RAM part). Only linked when _sbrk is referenced (malloc).
#include <kickos/board_config.h>
#ifndef KICKOS_HEAP_SIZE
#define KICKOS_HEAP_SIZE (64 * 1024)
#endif
static char s_heap[KICKOS_HEAP_SIZE];
static char* s_brk = s_heap;

static void* heap_bump(intptr_t incr)
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

void* _sbrk(intptr_t incr)
{
    return heap_bump(incr);
}

#ifdef __RX__
// SjLj atexit/EH registration references __dso_handle; the RX libc may not
// provide one. Weak so a libc that does still wins.
__attribute__((weak)) void* __dso_handle = nullptr;

// The RX psABI prefixes every C identifier with a leading underscore at the asm
// level, so the C `_sbrk` above mangles to asm `__sbrk` -- newlib references asm
// `_sbrk` and would otherwise fall through to libnosys sbrk (which pulls `_end`
// and breaks the app-window layout). A C function named `sbrk` mangles to asm
// `_sbrk`, satisfying newlib; it shares the one bump arena via heap_bump.
void* sbrk(intptr_t incr)
{
    return heap_bump(incr);
}
#endif

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
