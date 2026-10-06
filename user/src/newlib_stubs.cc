// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Newlib-compatible userspace porting layer: routes the newlib syscall interface, and
// so the toolchain's libstdc++/libsupc++, onto KickOS syscalls. NOT compiled for the
// sim, where host glibc already provides these symbols.

#include <kickos/sys.h>
#include <kickos/sys/emit.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>

extern "C"
{

// KickOS has no fd namespace: every fd goes to the console. MUST NOT cache how a call fared: a
// publish seats cap 0 after a send failed on it.
int _write(int, char const* buf, int len)
{
    if (len <= 0)
    {
        return 0;
    }
    // Short only for a non-blocking task: a short write would make newlib offer the rest again,
    // and a blocking writer's bytes are lost only to its own cancellation.
    size_t const took = kickos::stdout_write(buf, static_cast<size_t>(len));
    if (took == 0)
    {
        errno = EAGAIN;
        return -1;
    }
    return static_cast<int>(took);
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
// Newlib reads st_blksize as its buffer size where it has one, so the whole struct is written.
int _fstat(int, struct stat* st)
{
    struct stat const console{};
    *st = console;
    st->st_mode = S_IFCHR;
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

#if defined(__riscv) || defined(__x86_64__)
// Required to LINK the RISC-V full-C++ image: KEEPing .eh_frame (DWARF EH) retains the
// libc arc4random/getentropy FDEs, which pin the getentropy dependency chain against
// --gc-sections, so _getentropy must resolve; the x86_64 image is linked without
// --gc-sections at all. NOT a cryptographic source: KickOS exposes no HW RNG, and this is
// seeded off the monotonic clock only so the buffer is non-constant. Callers needing real
// entropy must wait for an RNG driver. Not ARM, which uses EHABI .ARM.exidx, keeps no
// .eh_frame, and never pulls this chain.
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
#elif defined(__XTENSA__) or defined(__RX__)
// libstdc++'s std::random_device and libc's arc4random reach getentropy, and no entropy
// source exists here.
int _getentropy(void*, size_t)
{
    errno = ENOSYS;
    return -1;
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

// Backs newlib's gettimeofday(), and so std::chrono::system_clock::now(). This
// toolchain's libstdc++ implements steady_clock::now() the same way, also through
// gettimeofday, so steady_clock is NOT monotonic here: code needing monotonic time must
// bypass libc and call kos_clock_now() directly (see kickcat's OS/KickOS/Time.cc).
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

// It must STAY empty: every chip .ld routes the app .fini_array into a section of its own
// and ASSERTs it empty, and newlib's own array bounds are weak-undefined in these images,
// so a destructor registered there has no runner either way.
void _fini(void)
{
}

// _sbrk and its bump arena live in newlib_sbrk.cc, not here: this TU is force-linked
// into every image by -Wl,-u,_exit, so a strong reference to _kickos_heap_start from
// here would defeat the heapless-board link error.

#if defined(__x86_64__) or defined(__RX__)
// The x86_64-elf and rx-elf newlibs' reentrant layer calls these names without the underscore;
// unaliased, stdout is lost silently.
int write(int fd, char const* buf, int len) __attribute__((alias("_write")));
int read(int, char*, int) __attribute__((alias("_read")));
int close(int) __attribute__((alias("_close")));
int isatty(int) __attribute__((alias("_isatty")));
int lseek(int, int, int) __attribute__((alias("_lseek")));
int fstat(int, struct stat*) __attribute__((alias("_fstat")));
int getpid(void) __attribute__((alias("_getpid")));
int kill(int, int) __attribute__((alias("_kill")));
int gettimeofday(struct timeval* tv, void*) __attribute__((alias("_gettimeofday")));
int getentropy(void* buf, size_t len) __attribute__((alias("_getentropy")));
#endif

#ifdef __RX__
// SjLj atexit/EH registration references __dso_handle and the RX libc may not provide
// one. Weak on purpose (allowlisted in tests/static/weak_allowlist.txt) so a libc that ships
// its own keeps ownership of the handle the atexit registrations key on.
__attribute__((weak)) void* __dso_handle = nullptr;
#endif

// Newlib brackets every arena mutation with __malloc_lock/__malloc_unlock. No-op weak
// stubs (allowlisted in tests/static/weak_allowlist.txt) so a full-C++ app that heap-allocates
// links and a thread-safe libc port can replace them; the pinned vendor toolchains are
// all built --disable-threads, so nothing else needs the guard.
//
// They must STAY no-ops: newlib takes this lock recursively (`_free_r` acquires it and
// calls `_malloc_trim_r`, which acquires it again), so a plain lock self-deadlocks and a
// re-entry detector fires on a valid free; and userspace has neither an owner identity
// nor a lock object two threads can name to build a recursive one from. Consequence: a
// full-C++ app that heap-allocates from more than one thread corrupts the arena
// silently. Keep such apps single-alloc-thread.
__attribute__((weak)) void __malloc_lock(void*)
{
}
__attribute__((weak)) void __malloc_unlock(void*)
{
}
}
