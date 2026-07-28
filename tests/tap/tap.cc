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
        constexpr int MAX_TESTS = 64;

        struct Entry
        {
            char const* name;
            TestFn fn;
        };

        Entry g_tests[MAX_TESTS];
        int g_count = 0;
        // Registrations dropped past MAX_TESTS. add() runs from static registrars,
        // possibly before main, so it can only record; run_all() turns a non-zero
        // count into a failing TAP line -- a silently dropped test would otherwise
        // be a gate that reports success without running.
        int g_dropped = 0;

        // Verdict of the test currently running. One slot for the failure diagnostic
        // and the skip reason: they are mutually exclusive, and the buffer is real
        // BSS on a 16 KiB-SRAM board.
        enum class Verdict : unsigned char
        {
            PASS,
            FAIL,
            SKIP
        };
        Verdict g_verdict = Verdict::PASS;
        char g_msg[192];

        // The one writer for the whole stream. Policy MIRRORS libc's _write
        // (user/src/newlib_stubs.cc): try THIS thread's cap index 0 -- the stdout
        // endpoint that kos_console_publish seats -- and fall back to the kernel debug
        // console for the REMAINDER only, when index 0 is empty (pre-publish,
        // -KOS_EBADF) or the driver died (-KOS_EPIPE). Both halves of that policy are
        // load-bearing and documented in place; this is a copy, not a new mechanism,
        // because the harness is freestanding (no libc stdio, no heap) -- keep the two
        // in step.
        //
        // kos_print alone is NOT enough, which is the bug this replaces: once a board's
        // service list publishes the console, console_emit DROPS every byte handed to
        // the kernel console (kernel/init/console.cc, USER_OWNED), so the entire TAP
        // stream -- verdicts and failure diagnostics alike -- went dark on precisely the
        // configuration the service-list boards ship.
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
                    // Remainder only: resending from the start would duplicate on the
                    // debug console the chunks the driver already took.
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
        // Probed with a ZERO-length send: a valid zero-length signal per <kickos/sys.h>,
        // so it discriminates the two postures while putting NO byte on the wire in
        // either -- unlike a 1-byte probe, which would inject a stray character into
        // the very stream we are about to emit.
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
        if (g_verdict != Verdict::PASS) // never downgrade a recorded failure
        {
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(g_msg, sizeof(g_msg), fmt, ap);
        va_end(ap);
        g_verdict = Verdict::SKIP;
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

    int run_all()
    {
        int plan = g_count;
        if (g_dropped > 0)
        {
            plan++; // one extra slot for the overflow verdict below
        }
        emitf("1..%d\n", plan);
        // Name the transport this run actually used. An operator staring at a silent
        // VCOM needs to tell "the driver is carrying TAP" from "the kernel console is",
        // and it is the one fact no individual test line reveals.
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
        for (int i = 0; i < g_count; i++)
        {
            g_verdict = Verdict::PASS;
            g_msg[0] = 0;
            g_tests[i].fn();
            if (g_verdict == Verdict::FAIL)
            {
                failed++;
                emitf("not ok %d - %s # %s\n", i + 1, g_tests[i].name, g_msg);
            }
            else if (g_verdict == Verdict::SKIP)
            {
                skipped++;
                emitf("ok %d - %s # SKIP %s\n", i + 1, g_tests[i].name, g_msg);
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
        // ALWAYS emitted, zero included: a gate keys its per-board skip budget off this
        // line, so its ABSENCE has to mean "truncated run", never "no skips". The
        // completion marker carries the count too, so a skip-padded green cannot read
        // as a full pass to a human -- while still matching the `# all tests passed`
        // substring every existing gate greps for.
        emitf("# skipped: %d\n", skipped);
        if (failed == 0 and skipped == 0)
        {
            emit("# all tests passed\n");
        }
        else if (failed == 0)
        {
            emitf("# all tests passed (%d skipped)\n", skipped);
        }
        else
        {
            emitf("# %d test(s) failed\n", failed);
        }
        return failed;
    }
}
