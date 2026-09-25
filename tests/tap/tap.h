// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A tiny freestanding TAP (Test Anything Protocol) harness for KickOS test apps.
// Runs on the target: the same binary emits `1..N` / `ok N - name` on the sim, on
// QEMU semihosting and on an MCU UART, so CTest keys off the TAP stream. No
// exceptions, no heap, no STL: a fixed-size static registry of function pointers;
// a test fails by recording a message (TAP_CHECK / tap::fail) or declares itself
// unrunnable here (tap::skip), both checked when the test function returns.
//
// Output route: every line goes through one publish-aware writer (stdout cap
// index 0, kernel-console fallback; see tap.cc emit()). Test bodies must use
// tap::diag/tap::skip, not kos::print: the kernel console drops everything once a
// board's service list hands the UART to a userspace driver.

#ifndef KICKOS_TESTS_TAP_TAP_H
#define KICKOS_TESTS_TAP_TAP_H

#include <stddef.h>

namespace tap
{
    using TestFn = void (*)();

    // A line that does not fit is emitted cut and marked, and the gate refuses the run.
    constexpr size_t LINE_BYTES = 224;
    constexpr size_t NAME_CHARS_MAX = 32;
    // What every directive carries whole beside any name. The widest line is
    // `ok NNNN - <name> # SKIP VACUOUS <reason>`, plus its newline and terminator.
    constexpr size_t REASON_CHARS_MAX = LINE_BYTES - 2 - (sizeof("ok 9999 - ") - 1)
                                        - NAME_CHARS_MAX - (sizeof(" # SKIP VACUOUS ") - 1);
    constexpr size_t UNBOUNDED = static_cast<size_t>(-1) / 2;

    // The most characters `fmt` expands to, every conversion at its widest. A %s is
    // UNBOUNDED: its argument is not in the format.
    constexpr size_t format_chars_max(char const* fmt)
    {
        size_t n = 0;
        size_t i = 0;
        while (fmt[i] != '\0')
        {
            if (fmt[i] != '%')
            {
                n++;
                i++;
                continue;
            }
            i++;
            bool wide = false;
            while (fmt[i] == 'l' or fmt[i] == 'z')
            {
                wide = true;
                i++;
            }
            char const c = fmt[i];
            if (c == '\0')
            {
                return n;
            }
            i++;
            if (c == 's')
            {
                return UNBOUNDED;
            }
            size_t width = 1;
            if (c == 'd' or c == 'i')
            {
                width = 11;
            }
            else if (c == 'u')
            {
                width = 10;
            }
            else if (c == 'x')
            {
                width = 8;
            }
            else if (c == 'p')
            {
                width = 2 + 2 * sizeof(void*);
            }
            if (wide and (c == 'd' or c == 'i' or c == 'u' or c == 'x'))
            {
                width = 20;
            }
            n += width;
        }
        return n;
    }

    // Register a test. Call before run_all(). A registration past MAX_TESTS is dropped,
    // and run_all() then emits an extra `not ok - tap_registry_overflow` and fails the
    // suite, so a truncated registry can never read as a clean run.
    void add_named(char const* name, TestFn fn);

    template <size_t N>
    void add(char const (&name)[N], TestFn fn)
    {
        static_assert(N - 1 <= NAME_CHARS_MAX,
                      "a TAP name past NAME_CHARS_MAX voids REASON_CHARS_MAX for every arm");
        add_named(name, fn);
    }

    // Mark the current test failed with a printf-style diagnostic. First failure
    // per test wins, and a failure always outranks a skip.
    void fail(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Mark the current test skipped with a printf-style reason: the harness emits
    // `ok N - name # SKIP <reason>` and counts it separately from the passes. Only
    // for a test that can assert nothing here; a test that ran its invariant and left
    // a sub-case unexercised is tap::partial, not this.
    // Like tap::fail it only records and does not return: follow it with `return`.
    void skip(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Mark the current test skipped for vacuity: the timing window its claim rests on did
    // not hold on this run, so the arm could assert nothing. The harness emits
    // `ok N - name # SKIP VACUOUS <reason>`, counts it apart from the ordinary skips and
    // states that count as `# vacuous: N`. It stays a SKIP directive on the wire on purpose:
    // a reader that does not know the category still refuses it rather than reading a pass.
    //
    // A gate permits one whatever its name and never expects one, which a provisioning skip
    // is not. Naming such an arm in EXPECT_SKIPS instead would make an arm gone permanently
    // vacuous read green forever, since a declared arm that did not skip is only a note.
    // The reason must say how far outside the window the run fell, or the permission hides
    // the same gap the missing detection did.
    // Like tap::skip it only records and does not return: follow it with `return`.
    // Call it through TAP_SKIP_VACUOUS, which bounds the reason at build time: the skip fires
    // only under load, which is where an over-long reason would first be seen.
    void skip_vacuous(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Mark the current test expected to fail: the arm is right and the tree is wrong, and the
    // defect is recorded with the change that will close it. The arm still runs; only the
    // reading of its result changes. The harness emits `not ok N - name # TODO <reason>` for
    // the failure, which a gate permits, and `ok N - name # TODO <reason>` when it passes,
    // which a gate reports: an arm that starts passing is the fix arriving, and the point of
    // the category is that nobody has to remember to come back.
    //
    // Not a skip. A skip says this board cannot host the arm and leaves the claim unasserted
    // forever; this says the claim is asserted, is failing, and is owed. Do not reach for it
    // to quieten an arm whose claim has become wrong: that arm is edited or deleted, because a
    // TODO on a false claim would announce a fix that is really a second defect.
    // Unlike skip and fail it does not record a verdict: place it anywhere in the arm and let
    // the arm run to its own conclusion.
    void todo(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Mark the current test partial with a printf-style reason: it ran its invariant
    // but left a sub-case unexercised on this board. The harness emits
    // `ok N - name # PARTIAL <reason>` and counts it separately; it stays a pass and
    // is never a skip. Do not report a partial with tap::diag: a `#` comment carries
    // no name a gate can key on, so the arm would be permitted implicitly everywhere.
    // First partial per test wins; a fail or a skip recorded later outranks it.
    void partial(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Emit a free-form TAP diagnostic (`# <text>`) on the harness's own route.
    void diag(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Register a repair to run after a test that failed, before the next one starts. A
    // failing TAP_CHECK returns mid-test, so a suite sharing state across tests strands
    // whatever the abandoned test had not consumed, and the next test reads it as its own:
    // one real failure is then reported as several. Runs on the failing path only. Call
    // before run_all(); one hook, last writer wins.
    void set_after_failure(TestFn fn);

    // Run every registered test in order, emit TAP, and return the number that
    // failed (0 == all passed). Skips, vacuity skips and partials are counted but are not
    // failures; the per-board lists of allowed ordinary skips and partials, by name, live in
    // the CTest gate (EXPECT_SKIPS / EXPECT_PARTIALS, checked by
    // tests/integration/check_tap_stream.sh). A vacuity skip has no list anywhere.
    int run_all();
}

#define TAP_SKIP_VACUOUS(fmt, ...)                                                  \
    do                                                                              \
    {                                                                               \
        static_assert(::tap::format_chars_max(fmt) <= ::tap::REASON_CHARS_MAX,      \
                      "this vacuity reason can overrun the TAP line: shorten it");  \
        ::tap::skip_vacuous(fmt __VA_OPT__(, ) __VA_ARGS__);                        \
    } while (0)

// Assert `cond`; on failure record "<file>:<line>: <expr>" and return from the current test,
// which the harness marks "not ok". Only valid inside a registered test function (void).
//
// The return is from the middle of the arm: an arm holding anything out of a shared pool must
// release it on that path too.
#define TAP_CHECK(cond)                                          \
    do                                                           \
    {                                                            \
        if (not(cond))                                           \
        {                                                        \
            ::tap::fail("%s:%d: %s", __FILE__, __LINE__, #cond); \
            return;                                              \
        }                                                        \
    } while (0)

#endif
