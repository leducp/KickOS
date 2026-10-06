// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Asserts the init spawned main with exactly the authority its composition declares,
// [memory, system, pinmux].
//
// The board's default composition lacks KOS_AUTH_PINMUX. Asserting a MISSING bit is refused
// would also pass with the declaration ignored, so arm 1 asserts a bit only the declaration
// can supply.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/fmt.h>

namespace
{
    // Out-of-range on every chip's {port,pin} encoding, so a real backend rejects it
    // before touching a mux register.
    constexpr uint32_t BAD_PORT = 0xFFFFu;
    constexpr uint32_t BAD_PIN = 0xFFFFu;

    int failures = 0;
    int arms = 0;

    void check(bool ok, char const* what)
    {
        if (ok)
        {
            arms = arms + 1;
            char msg[96];
            ksnprintf(msg, sizeof(msg), "[rootauth] ok - %s\n", what);
            kos::print(msg);
            return;
        }
        failures = failures + 1;
        char msg[96];
        ksnprintf(msg, sizeof(msg), "[rootauth] ERROR: %s\n", what);
        kos::print(msg);
    }

    void report_rc(char const* what, int rc)
    {
        char msg[96];
        ksnprintf(msg, sizeof(msg), "[rootauth]   %s rc=%d\n", what, rc);
        kos::print(msg);
    }
}

int main(int, char**)
{
    // -KOS_EPERM here means the declaration did not reach the spawn. Any other rc is the
    // backend answering past the authority gate.
    int rc = kos_pinmux_set(BAD_PORT, BAD_PIN, 0);
    report_rc("pinmux_set (declared bit)", rc);
    check(rc != -KOS_EPERM, "declared KOS_AUTH_PINMUX reached the spawn");

    // The authority gate precedes the cap lookup, so the handle must stay bogus: a
    // -KOS_EBADF here would mean the gate let the call through to the lookup.
    rc = kos_console_publish(-1, KOS_TASK_NONE);
    report_rc("console_publish (undeclared bit)", rc);
    check(rc == -KOS_EPERM, "undeclared KOS_AUTH_CONSOLE was not granted");

    // Must precede the narrow below, which drops KOS_AUTH_MEMORY. Leaks one block of
    // arch_ram_region_size(1); kos_ram_alloc never frees.
    void* mem = kos_ram_alloc(1);
    check(mem != nullptr, "declared KOS_AUTH_MEMORY reached kos_ram_alloc");

    rc = kos_cap_narrow(KOS_CAP_AUTHORITY, KOS_AUTH_SYSTEM);
    report_rc("cap_narrow to KOS_AUTH_SYSTEM", rc);
    check(rc == 0, "main narrowed its own cap further");

    // Separates a narrow that took effect from one that returned 0 and changed nothing.
    rc = kos_pinmux_set(BAD_PORT, BAD_PIN, 0);
    report_rc("pinmux_set after dropping it", rc);
    check(rc == -KOS_EPERM, "the just-dropped KOS_AUTH_PINMUX is now refused");

    if (failures != 0)
    {
        char msg[64];
        ksnprintf(msg, sizeof(msg), "[rootauth] FAIL (%d)\n", failures);
        kos::print(msg);
        return 1;
    }
    // The count comes from a counter, the `ok -` lines from one emit per arm: the gate
    // cross-checks the two, so output lost between them cannot read as a clean run.
    char msg[64];
    ksnprintf(msg, sizeof(msg), "[rootauth] PASS (%d arms)\n", arms);
    kos::print(msg);
    return 0;
}
