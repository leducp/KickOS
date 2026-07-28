// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A tiny freestanding TAP (Test Anything Protocol) harness for KickOS test apps.
// Runs on the target: the same binary emits `1..N` / `ok N - name` on the sim, on
// QEMU semihosting and on an MCU UART, so CTest keys off the TAP stream. No
// exceptions, no heap, no STL -- a fixed-size static registry of function pointers;
// a test fails by recording a message (TAP_CHECK / tap::fail) or declares itself
// unrunnable here (tap::skip), both checked when the test function returns.
//
// OUTPUT ROUTE: every line goes through one publish-aware writer (stdout cap
// index 0, kernel-console fallback; see tap.cc emit()). Test bodies must use
// tap::diag/tap::skip, NOT kos::print: the kernel console drops everything once a
// board's service list hands the UART to a userspace driver.

#ifndef KICKOS_TAP_H
#define KICKOS_TAP_H

namespace tap
{
    using TestFn = void (*)();

    // Register a test. Call before run_all(); silently ignored past capacity.
    void add(char const* name, TestFn fn);

    // Mark the CURRENT test failed with a printf-style diagnostic. First failure
    // per test wins, and a failure always outranks a skip.
    void fail(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Mark the CURRENT test SKIPPED with a printf-style reason: the harness emits
    // `ok N - name # SKIP <reason>` and counts it separately from the passes. Only
    // for a test that can assert NOTHING here; a test that ran its invariant and left
    // a sub-case unexercised stays `ok` and says so with tap::diag instead (see
    // PARTIAL in the selftest suite).
    // Like tap::fail it only records and does NOT return: follow it with `return`.
    void skip(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Emit a free-form TAP diagnostic (`# <text>`) on the harness's own route.
    void diag(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Run every registered test in order, emit TAP, and return the number that
    // FAILED (0 == all passed). Skips are counted but are not failures; the per-board
    // list of ALLOWED skips, by name, lives in the CTest gate
    // (tests/check_qemu_selftest.sh EXPECT_SKIPS).
    int run_all();
}

// Assert `cond`; on failure record "<file>:<line>: <expr>" and RETURN from the
// current test (so later steps don't run on bad state -- the harness marks it
// "not ok"). Only valid inside a registered test function (returns void).
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
