// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Newlib-compatible userspace porting layer: routes the newlib syscall interface, and
// so the toolchain's libstdc++/libsupc++, onto KickOS syscalls. NOT compiled for the
// sim, where host glibc already provides these symbols.

#include <kickos/sys.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

extern "C"
{

int _write(int fd, char const* buf, int len)
{
    // KickOS has no fd namespace: every fd goes to the console. Cap index 0 is
    // per-thread and fixed at spawn, so each call classifies itself afresh against its
    // own cap 0 and MUST NOT cache the outcome: cached state would let a pre-publish
    // thread's send failure poison a post-publish thread whose cap 0 is seated to the
    // endpoint. See the handover design (D5).
    (void)fd;
    if (len <= 0)
    {
        return 0;
    }
    size_t const total = static_cast<size_t>(len);
    size_t sent = 0;
    while (sent < total)
    {
        size_t chunk = total - sent;
        if (chunk > KOS_EP_MSG_MAX)
        {
            chunk = KOS_EP_MSG_MAX;
        }
        long const r = kos_send(0, buf + sent, chunk); // index 0 == the stdout endpoint cap
        // r == 0 (a receiver with no buffer) would spin forever: fall back, don't retry.
        if (r <= 0)
        {
            // Pre-publish (index 0 empty, -KOS_EBADF) or the driver died (-KOS_EPIPE).
            // Fall back on the REMAINDER only: resending the whole buffer would duplicate
            // the chunks already delivered to the driver. Return the FULL len even so,
            // because a short write would make newlib retry and re-send the bytes IPC
            // already accepted.
            kos_kconsole_write(buf + sent, total - sent);
            return len;
        }
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
// Required to LINK the RISC-V full-C++ image: KEEPing .eh_frame (DWARF EH) retains the
// libc arc4random/getentropy FDEs, which pin the getentropy dependency chain against
// --gc-sections, so _getentropy must resolve. NOT a cryptographic source: KickOS
// exposes no HW RNG, and this is seeded off the monotonic clock only so the buffer is
// non-constant. Callers needing real entropy must wait for an RNG driver. RISC-V only,
// because ARM uses EHABI .ARM.exidx, keeps no .eh_frame, and never pulls this chain.
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

// exit() reaches _exit above only through __libc_fini_array, which calls this. It must
// STAY empty: every chip .ld routes the app .fini_array into a section of its own and
// ASSERTs it empty, and newlib's own array bounds are weak-undefined in these images, so
// a destructor registered there has no runner either way.
void _fini(void)
{
}

// _sbrk and its bump arena live in newlib_sbrk.cc, not here: this TU is force-linked
// into every image by -Wl,-u,_exit, so a strong reference to _kickos_heap_start from
// here would defeat the heapless-board link error.

#ifdef __RX__
// SjLj atexit/EH registration references __dso_handle and the RX libc may not provide
// one. Weak on purpose (allowlisted in tests/static/weak_allowlist.txt) so a libc that ships
// its own keeps ownership of the handle the atexit registrations key on.
__attribute__((weak)) void* __dso_handle = nullptr;
#endif

#ifdef __XTENSA__
#include <sys/reent.h>
// esp-elf newlib resolves _REENT through __getreent(), NOT through _impure_ptr, and
// ships a WEAK fallback that returns NULL: without this override every stdio call
// dereferences null and takes a LoadProhibited exception (EXCCAUSE 0x1c, EXCVADDR 0x28
// from _puts_r's CHECK_INIT, which loads before its null test). The toolchain only
// emits a .gnu.warning, so an otherwise green link is the sole hint. _impure_data
// itself is present and statically initialised by libc; returning it makes stdio
// process-global, so errno is shared across threads as on the ARM and RX ports.
extern "C" struct _reent* __getreent(void)
{
    return _GLOBAL_REENT;
}
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
