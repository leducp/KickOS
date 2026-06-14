// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A tiny freestanding TAP (Test Anything Protocol) harness for KickOS test apps.
// Runs on the target: the same binary emits `1..N` / `ok N - name` over the
// debug console on the sim and (later) on an MCU UART, so CTest keys off the TAP
// stream. No exceptions, no heap, no STL -- a fixed-size static registry of
// function pointers; a test fails by recording a message (TAP_CHECK / tap::fail),
// checked when the test function returns.

#ifndef KICKOS_TAP_H
#define KICKOS_TAP_H

namespace tap
{
    using TestFn = void (*)();

    // Register a test. Call before run_all(); silently ignored past capacity.
    void add(char const* name, TestFn fn);

    // Mark the CURRENT test failed with a printf-style diagnostic. First failure
    // per test wins.
    void fail(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Run every registered test in order, emit TAP to the console, and return the
    // number that failed (0 == all passed).
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
