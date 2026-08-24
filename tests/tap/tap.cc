// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "tap.h"

#include <stdarg.h>

#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/libc/string.h>

namespace tap
{
    namespace
    {
        // 8 bytes each, in test images only. An image that knows its own plan states it as
        // KICKOS_TAP_MAX_TESTS and pays for that many; the 128 below is the fleet ceiling a
        // caller who states nothing falls back to, and on a 16 KiB-SRAM board it is 1 KiB of
        // .bss under the arena. Registrations past the cap are counted, not silently lost:
        // run_all() turns g_dropped into a failing tap_registry_overflow line.
#ifndef KICKOS_TAP_MAX_TESTS
#define KICKOS_TAP_MAX_TESTS 128
#endif
        constexpr int MAX_TESTS = KICKOS_TAP_MAX_TESTS;

        struct Entry
        {
            char const* name;
            TestFn fn;
        };

        Entry g_tests[MAX_TESTS];
        int g_count = 0;
        // Registrations dropped past MAX_TESTS. add() may run before main, so it can
        // only record; run_all() turns a non-zero count into a failing TAP line.
        int g_dropped = 0;

        // One buffer for the failure diagnostic, the skip reason and the partial reason:
        // mutually exclusive, and real BSS on a 16 KiB-SRAM board.
        enum class Verdict : unsigned char
        {
            PASS,
            PARTIAL,
            SKIP,
            FAIL
        };
        Verdict g_verdict = Verdict::PASS;
        char g_msg[192];

        // Repair for the failing path, or null.
        TestFn g_after_failure = nullptr;

        // The one writer for the whole stream, and the third copy of a policy libc's
        // _write (user/src/newlib_stubs.cc) and <kickos/sys/emit.h> also carry: keep them in
        // step. Try this thread's stdout cap at index 0, fall back to the kernel debug
        // console for the remainder when index 0 is empty (-KOS_EBADF) or the driver died
        // (-KOS_EPIPE). kos_print alone is not enough, because console_emit drops every byte
        // handed to the kernel console once a service list publishes it
        // (kernel/init/console.cc, USER_OWNED).
        void emit(char const* s)
        {
            size_t const total = strlen(s);
            size_t sent = 0;
            while (sent < total)
            {
                size_t chunk = total - sent;
                if (chunk > KOS_EP_MSG_MAX)
                {
                    chunk = KOS_EP_MSG_MAX;
                }
                long const r = kos_send(0, s + sent, chunk); // index 0 == the stdout endpoint cap
                // r == 0 (a receiver with no buffer) would spin forever: fall back, don't retry.
                if (r <= 0)
                {
                    // Remainder only: resending from the start would duplicate the
                    // chunks the driver already took.
                    kos_kconsole_write(s + sent, total - sent);
                    return;
                }
                sent += static_cast<size_t>(r);
            }
        }

        void emitf(char const* fmt, ...)
        {
            char b[224];
            va_list ap;
            va_start(ap, fmt);
            kvsnprintf(b, sizeof(b), fmt, ap);
            va_end(ap);
            emit(b);
        }

        // Is this thread's stdout cap seated (a service list published the console)?
        // A zero-length send is a valid signal per <kickos/sys.h> and puts no byte on
        // the wire in either posture, unlike a 1-byte probe.
        bool stdout_published()
        {
            return kos_send(0, "", 0) >= 0;
        }
    }

    void add(char const* name, TestFn fn)
    {
        if (g_count < MAX_TESTS)
        {
            g_tests[g_count].name = name;
            g_tests[g_count].fn = fn;
            g_count++;
        }
        else
        {
            g_dropped++;
        }
    }

    void fail(char const* fmt, ...)
    {
        if (g_verdict == Verdict::FAIL) // first failure per test wins
        {
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(g_msg, sizeof(g_msg), fmt, ap);
        va_end(ap);
        g_verdict = Verdict::FAIL; // outranks a skip recorded earlier
    }

    void skip(char const* fmt, ...)
    {
        if (g_verdict == Verdict::FAIL or g_verdict == Verdict::SKIP)
        {
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(g_msg, sizeof(g_msg), fmt, ap);
        va_end(ap);
        g_verdict = Verdict::SKIP;
    }

    void partial(char const* fmt, ...)
    {
        if (g_verdict != Verdict::PASS) // outranked by a fail or a skip; first partial wins
        {
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(g_msg, sizeof(g_msg), fmt, ap);
        va_end(ap);
        g_verdict = Verdict::PARTIAL;
    }

    void diag(char const* fmt, ...)
    {
        char b[200];
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        emitf("# %s\n", b);
    }

    void set_after_failure(TestFn fn) { g_after_failure = fn; }

    int run_all()
    {
        int plan = g_count;
        if (g_dropped > 0)
        {
            plan++; // one extra slot for the overflow verdict below
        }
        emitf("1..%d\n", plan);
        if (stdout_published())
        {
            diag("tap route: stdout endpoint -> console driver (service list published)");
        }
        else
        {
            diag("tap route: kernel debug console (stdout not published)");
        }
        int failed = 0;
        int skipped = 0;
        int partials = 0;
        for (int i = 0; i < g_count; i++)
        {
            g_verdict = Verdict::PASS;
            g_msg[0] = 0;
            g_tests[i].fn();
            if (g_verdict == Verdict::FAIL)
            {
                failed++;
                emitf("not ok %d - %s # %s\n", i + 1, g_tests[i].name, g_msg);
                if (g_after_failure != nullptr)
                {
                    g_after_failure();
                }
            }
            else if (g_verdict == Verdict::SKIP)
            {
                skipped++;
                emitf("ok %d - %s # SKIP %s\n", i + 1, g_tests[i].name, g_msg);
            }
            else if (g_verdict == Verdict::PARTIAL)
            {
                partials++;
                emitf("ok %d - %s # PARTIAL %s\n", i + 1, g_tests[i].name, g_msg);
            }
            else
            {
                emitf("ok %d - %s\n", i + 1, g_tests[i].name);
            }
        }
        if (g_dropped > 0)
        {
            failed++;
            emitf("not ok %d - tap_registry_overflow # %d registration(s) dropped past MAX_TESTS=%d\n",
                  g_count + 1, g_dropped, MAX_TESTS);
        }
        // Both always emitted, zero included: a gate reconciles its by-name permission
        // set against these counts, so an absent line must mean "truncated run", never
        // "none of those". The completion marker must keep the `# all tests passed`
        // substring the gates grep for.
        emitf("# skipped: %d\n", skipped);
        emitf("# partial: %d\n", partials);
        if (failed == 0 and skipped == 0 and partials == 0)
        {
            emit("# all tests passed\n");
        }
        else if (failed == 0)
        {
            emitf("# all tests passed (%d skipped, %d partial)\n", skipped, partials);
        }
        else
        {
            emitf("# %d test(s) failed\n", failed);
        }
        return failed;
    }
}
