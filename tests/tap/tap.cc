// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "tap.h"

#include <stdarg.h>

#include <kickos/sys.h>
#include <kickos/sys/emit.h>
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
        static_assert(MAX_TESTS <= 9999, "REASON_CHARS_MAX budgets a four-digit test index");

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
            SKIP_VACUOUS,
            FAIL
        };
        Verdict g_verdict = Verdict::PASS;
        char g_msg[192];
        static_assert(sizeof(g_msg) > REASON_CHARS_MAX);
        // Orthogonal to the verdict, and that is the whole of the category: the arm runs and
        // reaches its own conclusion, and this only decides how that conclusion is read. Its
        // reason needs storage of its own because a failing arm has already written g_msg.
        bool g_todo = false;
        char g_todo_msg[192];

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
                    // Close on EPIPE only, and emit.h states why: the peer closing does
                    // not free this side, and -KOS_EBADF is pre-publish with nothing to close.
                    if (r == -KOS_EPIPE)
                    {
                        (void)kos_handle_close(KOS_CAP_STDOUT);
                    }
                    // Remainder only: resending from the start would duplicate the
                    // chunks the driver already took.
                    kickos::kconsole_write_all(s + sent, total - sent);
                    return;
                }
                sent += static_cast<size_t>(r);
            }
        }

        // Stands in for the tail of a line the assembly buffer could not hold, and carries
        // the newline so a cut line still ends where a reader counts.
        constexpr char CUT[] = "<TRUNCATED>\n";
        constexpr char REASON_CUT[] = "<TRUNCATED>";

        // A reason cut here and never marked would reach the stream looking whole, since the
        // line it lands in can still fit.
        void record(char* dst, size_t cap, char const* fmt, va_list ap)
        {
            int const w = kvsnprintf(dst, cap, fmt, ap);
            if (w >= 0 and static_cast<size_t>(w) >= cap)
            {
                memcpy(dst + cap - sizeof(REASON_CUT), REASON_CUT, sizeof(REASON_CUT));
            }
        }

        // The newline is the emitter's, never a caller's: a
        // line that fills the buffer would otherwise lose its own and run into the result
        // line after it, which leaves that one uncountable at line start and drops it from
        // every by-line parse. A format string that carries a '\n' of its own gets a blank
        // line, not a lost one.
        // `pfx` goes into the same buffer rather than a write of its own: above one core a
        // second write can land inside another node's line.
        void emitv(char const* pfx, size_t pfxlen, char const* fmt, va_list ap)
        {
            char b[LINE_BYTES];
            memcpy(b, pfx, pfxlen);
            int const w = kvsnprintf(b + pfxlen, sizeof(b) - pfxlen, fmt, ap);
            size_t const want = pfxlen + static_cast<size_t>(w);
            if (want + 2 > sizeof(b))
            {
                memcpy(b + sizeof(b) - sizeof(CUT), CUT, sizeof(CUT));
            }
            else
            {
                b[want] = '\n';
                b[want + 1] = '\0';
            }
            emit(b);
        }

        void emitf(char const* fmt, ...)
        {
            va_list ap;
            va_start(ap, fmt);
            emitv("", 0, fmt, ap);
            va_end(ap);
        }

        // The two skip categories rank together: first skip of either kind wins, and a fail
        // recorded later still outranks both.
        void record_skip(Verdict v, char const* fmt, va_list ap)
        {
            if (g_verdict == Verdict::FAIL or g_verdict == Verdict::SKIP
                or g_verdict == Verdict::SKIP_VACUOUS)
            {
                return;
            }
            record(g_msg, sizeof(g_msg), fmt, ap);
            g_verdict = v;
        }

        // Is this thread's stdout cap seated (a service list published the console)?
        // A zero-length send is a valid signal per <kickos/sys.h> and puts no byte on
        // the wire in either posture, unlike a 1-byte probe.
        bool stdout_published()
        {
            return kos_send(0, "", 0) >= 0;
        }
    }

    void add_named(char const* name, TestFn fn)
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
        record(g_msg, sizeof(g_msg), fmt, ap);
        va_end(ap);
        g_verdict = Verdict::FAIL; // outranks a skip recorded earlier
    }

    void skip(char const* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        record_skip(Verdict::SKIP, fmt, ap);
        va_end(ap);
    }

    void skip_vacuous(char const* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        record_skip(Verdict::SKIP_VACUOUS, fmt, ap);
        va_end(ap);
    }

    void todo(char const* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        record(g_todo_msg, sizeof(g_todo_msg), fmt, ap);
        va_end(ap);
        g_todo = true;
    }

    void partial(char const* fmt, ...)
    {
        if (g_verdict != Verdict::PASS) // outranked by a fail or a skip; first partial wins
        {
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        record(g_msg, sizeof(g_msg), fmt, ap);
        va_end(ap);
        g_verdict = Verdict::PARTIAL;
    }

    void diag(char const* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        emitv("# ", 2, fmt, ap);
        va_end(ap);
    }

    void set_after_failure(TestFn fn) { g_after_failure = fn; }

    int run_all()
    {
        int plan = g_count;
        if (g_dropped > 0)
        {
            plan++; // one extra slot for the overflow verdict below
        }
        emitf("1..%d", plan);
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
        int vacuous = 0;
        int partials = 0;
        int todo_owed = 0;
        int todo_fixed = 0;
        for (int i = 0; i < g_count; i++)
        {
            g_verdict = Verdict::PASS;
            g_msg[0] = 0;
            g_todo = false;
            g_todo_msg[0] = 0;
            g_tests[i].fn();
            // Read before the verdict is, because a TODO arm's failure is not the run's.
            if (g_todo)
            {
                if (g_verdict == Verdict::FAIL)
                {
                    todo_owed++;
                    emitf("not ok %d - %s # TODO %s", i + 1, g_tests[i].name, g_todo_msg);
                }
                else
                {
                    // The fix arriving. Not counted as a failure here: the gate is what
                    // decides, and it reads this line by name.
                    todo_fixed++;
                    emitf("ok %d - %s # TODO %s", i + 1, g_tests[i].name, g_todo_msg);
                }
                continue;
            }
            if (g_verdict == Verdict::FAIL)
            {
                failed++;
                emitf("not ok %d - %s # %s", i + 1, g_tests[i].name, g_msg);
                if (g_after_failure != nullptr)
                {
                    g_after_failure();
                }
            }
            // One directive with a sub-category, and the sub-category is the harness's: an
            // arm can neither forge nor misspell it into its own reason string.
            else if (g_verdict == Verdict::SKIP or g_verdict == Verdict::SKIP_VACUOUS)
            {
                char const* directive = "SKIP";
                if (g_verdict == Verdict::SKIP)
                {
                    skipped++;
                }
                else
                {
                    vacuous++;
                    directive = "SKIP VACUOUS";
                }
                emitf("ok %d - %s # %s %s", i + 1, g_tests[i].name, directive, g_msg);
            }
            else if (g_verdict == Verdict::PARTIAL)
            {
                partials++;
                emitf("ok %d - %s # PARTIAL %s", i + 1, g_tests[i].name, g_msg);
            }
            else
            {
                emitf("ok %d - %s", i + 1, g_tests[i].name);
            }
        }
        if (g_dropped > 0)
        {
            failed++;
            emitf("not ok %d - tap_registry_overflow # %d registration(s) dropped past MAX_TESTS=%d",
                  g_count + 1, g_dropped, MAX_TESTS);
        }
        // All five lines are always emitted, zero included: a gate reconciles its by-name
        // permission set against these counts, so an absent line must mean "truncated run",
        // never "none of those". `# vacuous: N` carries no permission set to reconcile and is
        // stated for the same reason in reverse, to prove the gate's marker parse still
        // matches. The three counts are disjoint; `todo-fixed` is the one a gate acts on. The
        // completion marker must keep the `# all tests passed` substring the gates grep for.
        emitf("# todo: %d", todo_owed);
        emitf("# todo-fixed: %d", todo_fixed);
        emitf("# skipped: %d", skipped);
        emitf("# vacuous: %d", vacuous);
        emitf("# partial: %d", partials);
        if (failed == 0 and skipped == 0 and vacuous == 0 and partials == 0)
        {
            emitf("# all tests passed");
        }
        else if (failed == 0)
        {
            emitf("# all tests passed (%d skipped, %d vacuous, %d partial)", skipped, vacuous,
                  partials);
        }
        else
        {
            emitf("# %d test(s) failed", failed);
        }
        return failed;
    }
}
