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
// OUTPUT ROUTE: every line goes through one publish-aware writer (stdout cap
// index 0, kernel-console fallback; see tap.cc emit()). Test bodies must use
// tap::diag/tap::skip, NOT kos::print: the kernel console drops everything once a
// board's service list hands the UART to a userspace driver.

#ifndef KICKOS_TESTS_TAP_TAP_H
#define KICKOS_TESTS_TAP_TAP_H

namespace tap
{
    using TestFn = void (*)();

    // Register a test. Call before run_all(). A registration past MAX_TESTS is dropped,
    // and run_all() then emits an extra `not ok - tap_registry_overflow` and fails the
    // suite, so a truncated registry can never read as a clean run.
    void add(char const* name, TestFn fn);

    // Mark the CURRENT test failed with a printf-style diagnostic. First failure
    // per test wins, and a failure always outranks a skip.
    void fail(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Mark the CURRENT test SKIPPED with a printf-style reason: the harness emits
    // `ok N - name # SKIP <reason>` and counts it separately from the passes. Only
    // for a test that can assert NOTHING here; a test that ran its invariant and left
    // a sub-case unexercised is tap::partial, not this.
    // Like tap::fail it only records and does NOT return: follow it with `return`.
    void skip(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Mark the CURRENT test PARTIAL with a printf-style reason: it ran its invariant
    // but left a sub-case unexercised on this board. The harness emits
    // `ok N - name # PARTIAL <reason>` and counts it separately; it stays a PASS and
    // is never a skip. Do NOT report a partial with tap::diag: a `#` comment carries
    // no name a gate can key on, so the arm would be permitted implicitly everywhere.
    // First partial per test wins; a fail or a skip recorded later outranks it.
    void partial(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Emit a free-form TAP diagnostic (`# <text>`) on the harness's own route.
    void diag(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Register a repair to run after a test that FAILED, before the next one starts. A
    // failing TAP_CHECK RETURNS mid-test, so a suite sharing state across tests strands
    // whatever the abandoned test had not consumed, and the next test reads it as its own:
    // one real failure is then reported as several. Runs on the failing path ONLY. Call
    // before run_all(); one hook, last writer wins.
    void set_after_failure(TestFn fn);

    // Run every registered test in order, emit TAP, and return the number that
    // FAILED (0 == all passed). Skips and partials are counted but are not failures;
    // the per-board lists of ALLOWED ones, by name, live in the CTest gate
    // (EXPECT_SKIPS / EXPECT_PARTIALS, checked by tests/integration/check_tap_stream.sh).
    int run_all();
}

// Assert `cond`; on failure record "<file>:<line>: <expr>" and RETURN from the current test,
// which the harness marks "not ok". Only valid inside a registered test function (void).
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
