// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Newlib-compatible userspace porting layer: routes the newlib syscall interface, and
// so the toolchain's libstdc++/libsupc++, onto KickOS syscalls. NOT compiled for the
// sim, where host glibc already provides these symbols.

#include <kickos/arch/arch.h>
#include <kickos/sys.h>
#include <kickos/sys/emit.h>
#include <kickos/sys/user_cas.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

// newlib states the POSIX clocks only where the toolchain configures them.
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC ((clockid_t)4)
#endif

// POSIX.1-2024's <limits.h> value, which newlib does not state.
#ifndef GETENTROPY_MAX
#define GETENTROPY_MAX 256
#endif

extern "C"
{
int clock_gettime(clockid_t id, struct timespec* ts);
int clock_settime(clockid_t id, struct timespec const* ts);
int nanosleep(struct timespec const* req, struct timespec* rem);
#if defined(__x86_64__)
bool kickos_user_rdseed_present(void);
// One RDSEED; false when the processor has no seed ready.
bool kickos_user_rdseed(uint64_t* word);
#endif
}

namespace
{
    constexpr uint64_t NS_PER_SEC = 1000000000ull;

    // A time a timespec names in nanoseconds, saturated at the largest; false for a negative
    // time or a nanosecond field outside [0, 1e9).
    bool timespec_ns(struct timespec const* ts, uint64_t* ns)
    {
        if (ts->tv_sec < 0 or ts->tv_nsec < 0 or ts->tv_nsec >= static_cast<long>(NS_PER_SEC))
        {
            return false;
        }
        uint64_t const sec = static_cast<uint64_t>(ts->tv_sec);
        uint64_t const nsec = static_cast<uint64_t>(ts->tv_nsec);
        *ns = UINT64_MAX;
        if (sec <= (UINT64_MAX - nsec) / NS_PER_SEC)
        {
            *ns = sec * NS_PER_SEC + nsec;
        }
        return true;
    }

#if defined(__x86_64__)
    // A seed not ready is a delay, never a failure: the draw repeats until it is.
    void rdseed_fill(unsigned char* out, size_t len)
    {
        size_t done = 0;
        while (done < len)
        {
            uint64_t word = 0;
            while (not kickos_user_rdseed(&word))
            {
                __asm__ volatile("pause");
            }
            for (uint32_t byte = 0; byte < 8 and done < len; byte++)
            {
                out[done] = static_cast<unsigned char>(word >> (8u * byte));
                done = done + 1;
            }
        }
    }
#endif

    int posix_errno(int32_t error)
    {
        switch (error)
        {
            // The kernel's answer to a non-blocking task where it would wait.
            case -KOS_ETIMEDOUT:
            {
                return EAGAIN;
            }
            case -KOS_EFAULT:
            {
                return EFAULT;
            }
            case -KOS_ECANCELED:
            {
                return ECANCELED;
            }
            case -KOS_EACCES:
            {
                return EACCES;
            }
            case -KOS_EPERM:
            {
                return EPERM;
            }
            case -KOS_EINVAL:
            {
                return EINVAL;
            }
            default:
            {
                return EIO;
            }
        }
    }
}

extern "C"
{

// Fds 0 to 2 are the console, a terminal that reads nothing; no call opens another. MUST NOT
// cache how a write fared: a publish seats cap 0 after a send failed on it.
int _write(int fd, char const* buf, int len)
{
    if ((fd != 1 and fd != 2) or not kickos::console_fd_open(fd))
    {
        errno = EBADF;
        return -1;
    }
    if (len <= 0)
    {
        return 0;
    }
    kickos::WriteResult const r = kickos::stdout_write(buf, static_cast<size_t>(len));
    if (r.sent > 0)
    {
        return static_cast<int>(r.sent);
    }
    errno = posix_errno(r.error);
    return -1;
}

int _read(int fd, char*, int)
{
    if (fd != 0 or not kickos::console_fd_open(fd))
    {
        errno = EBADF;
        return -1;
    }
    return 0;
}
// No path names a file: fopen, freopen and the rest fail as for a file that does not exist.
int _open(char const*, int, ...)
{
    errno = ENOENT;
    return -1;
}
int _close(int fd)
{
    if (fd >= 0 and fd <= 2)
    {
        uint32_t const bit = 1u << fd;
        uint32_t held = __atomic_load_n(&kickos::console_fds(), __ATOMIC_RELAXED);
        while ((held & bit) != 0u)
        {
            uint32_t const seen = kickos_user_cas(&kickos::console_fds(), held, held & ~bit);
            if (seen == held)
            {
                return 0;
            }
            held = seen;
        }
    }
    errno = EBADF;
    return -1;
}
int _isatty(int fd)
{
    if (not kickos::console_fd_open(fd))
    {
        errno = EBADF;
        return 0;
    }
    return 1;
}
int _lseek(int fd, int, int)
{
    if (not kickos::console_fd_open(fd))
    {
        errno = EBADF;
        return -1;
    }
    errno = ESPIPE;
    return -1;
}
// A character device, so newlib line-buffers a stream on it.
int _fstat(int fd, struct stat* st)
{
    if (not kickos::console_fd_open(fd))
    {
        errno = EBADF;
        return -1;
    }
    struct stat const console{};
    *st = console;
    st->st_mode = S_IFCHR;
    return 0;
}
int _getpid(void)
{
    return 1;
}

void _exit(int code) __attribute__((noreturn));

// A signal to the caller's own process takes its default action, newlib's raise() having run
// any handler: the task ends with 128 + sig where that action ends a process, so abort() ends it
// with 134. No signal reaches another process.
int _kill(int pid, int sig)
{
    if (sig < 0 or sig >= NSIG)
    {
        errno = EINVAL;
        return -1;
    }
    if (pid != _getpid() and pid != 0)
    {
        errno = ESRCH;
        return -1;
    }
    switch (sig)
    {
        case 0:
        case SIGCHLD:
        case SIGCONT:
        case SIGURG:
        case SIGWINCH:
        {
            return 0;
        }
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
        {
            errno = EINVAL;
            return -1;
        }
        default:
        {
            _exit(128 + sig);
        }
    }
}

// RDSEED where the x86_64 processor offers it; no other source is reached. arc4random raises
// SIGKILL when this fails.
int _getentropy(void* buf, size_t len)
{
#if defined(__x86_64__)
    if (kickos_user_rdseed_present())
    {
        if (len > GETENTROPY_MAX)
        {
            errno = EINVAL;
            return -1;
        }
        rdseed_fill(static_cast<unsigned char*>(buf), len);
        return 0;
    }
#else
    (void)buf;
    (void)len;
#endif
    errno = ENOSYS;
    return -1;
}

// Backs time().
int _gettimeofday(struct timeval* tv, void*)
{
    if (tv != nullptr)
    {
        uint64_t const ns = kos_clock_realtime();
        tv->tv_sec = static_cast<time_t>(ns / NS_PER_SEC);
        tv->tv_usec = static_cast<suseconds_t>((ns % NS_PER_SEC) / 1000u);
    }
    return 0;
}

int clock_gettime(clockid_t id, struct timespec* ts)
{
    uint64_t ns = 0;
    if (id == CLOCK_REALTIME)
    {
        ns = kos_clock_realtime();
    }
    else if (id == CLOCK_MONOTONIC)
    {
        ns = kos_clock_now();
    }
    else
    {
        errno = EINVAL;
        return -1;
    }
    ts->tv_sec = static_cast<time_t>(ns / NS_PER_SEC);
    ts->tv_nsec = static_cast<long>(ns % NS_PER_SEC);
    return 0;
}

int clock_settime(clockid_t id, struct timespec const* ts)
{
    uint64_t ns = 0;
    if (id != CLOCK_REALTIME or not timespec_ns(ts, &ns))
    {
        errno = EINVAL;
        return -1;
    }
    int const rc = kos_clock_set_realtime(ns);
    if (rc != 0)
    {
        errno = posix_errno(rc);
        return -1;
    }
    return 0;
}

// The sleep is never broken, so `rem` is never written.
int nanosleep(struct timespec const* req, struct timespec*)
{
    uint64_t ns = 0;
    if (not timespec_ns(req, &ns))
    {
        errno = EINVAL;
        return -1;
    }
    kos_sleep_ns(ns);
    return 0;
}

// The whole task ends, its other threads stopped, with `code` its status.
void _exit(int code)
{
    arch_syscall(KOS_SYS_EXIT, static_cast<uintptr_t>(code), KOS_EXIT_TASK, 0, 0);
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
int open(char const*, int, ...) __attribute__((alias("_open")));
int close(int) __attribute__((alias("_close")));
int isatty(int) __attribute__((alias("_isatty")));
int lseek(int, int, int) __attribute__((alias("_lseek")));
int fstat(int, struct stat*) __attribute__((alias("_fstat")));
int getpid(void) __attribute__((alias("_getpid")));
int kill(int, int) __attribute__((alias("_kill")));
int gettimeofday(struct timeval* tv, void*) __attribute__((alias("_gettimeofday")));
int getentropy(void* buf, size_t len) __attribute__((alias("_getentropy")));
#endif

// The handle the C++ runtime's atexit registrations key on, which no KickOS image's start files
// define. Weak (allowlisted in tests/static/weak_allowlist.txt) so a libc that ships its own
// keeps it.
__attribute__((weak)) void* __dso_handle = nullptr;
}
