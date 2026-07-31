// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Asserts kickos_default_init_run narrowed root's authority cap to this app's declared
// mask before calling main.
//
// The mask below adds KOS_AUTH_PINMUX, which the fallback (KOS_AUTH_MEMORY |
// KOS_AUTH_SYSTEM) lacks. Asserting a MISSING bit is refused would also pass with the
// declaration ignored, so arm 1 asserts a bit only the declaration can supply.
//
// KOS_AUTH_SYSTEM is mandatory here: main returns, and root_entry's kos_shutdown
// panics "root: shutdown refused" without it.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>

using kickos::emit;

KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_PINMUX);

namespace
{
    // Out-of-range on every chip's {port,pin} encoding, so a real backend rejects it
    // before touching a mux register.
    constexpr uint32_t BAD_PORT = 0xFFFFu;
    constexpr uint32_t BAD_PIN = 0xFFFFu;

    int failures = 0;

    void check(bool ok, char const* what)
    {
        if (ok)
        {
            char msg[96];
            ksnprintf(msg, sizeof(msg), "[rootauth] ok - %s\n", what);
            emit(msg);
            return;
        }
        failures = failures + 1;
        char msg[96];
        ksnprintf(msg, sizeof(msg), "[rootauth] ERROR: %s\n", what);
        emit(msg);
    }

    void report_rc(char const* what, int rc)
    {
        char msg[96];
        ksnprintf(msg, sizeof(msg), "[rootauth]   %s rc=%d\n", what, rc);
        emit(msg);
    }
}

int main(int, char**)
{
    // -KOS_EPERM here means the declaration was ignored and root ran on the fallback.
    // Any other rc is the backend answering past the authority gate.
    int rc = kos_pinmux_set(BAD_PORT, BAD_PIN, 0);
    report_rc("pinmux_set (declared bit)", rc);
    check(rc != -KOS_EPERM, "declared KOS_AUTH_PINMUX survived the narrow");

    // The authority gate precedes the cap lookup, so the handle must stay bogus: a
    // -KOS_EBADF here would mean the gate let the call through to the lookup.
    rc = kos_console_publish(-1);
    report_rc("console_publish (undeclared bit)", rc);
    check(rc == -KOS_EPERM, "undeclared KOS_AUTH_CONSOLE was dropped by the narrow");

    // Must precede the narrow below, which drops KOS_AUTH_MEMORY. Leaks one 16-byte
    // bump block; kos_ram_alloc never frees.
    void* mem = kos_ram_alloc(1);
    check(mem != nullptr, "declared KOS_AUTH_MEMORY reached kos_ram_alloc");

    // Keeps KOS_AUTH_SYSTEM: main returns, and root_entry's kos_shutdown needs it.
    rc = kos_cap_narrow(KOS_CAP_AUTHORITY, KOS_AUTH_SYSTEM);
    report_rc("cap_narrow to KOS_AUTH_SYSTEM", rc);
    check(rc == 0, "root narrowed its own cap further");

    // Separates a narrow that took effect from one that returned 0 and changed nothing.
    rc = kos_pinmux_set(BAD_PORT, BAD_PIN, 0);
    report_rc("pinmux_set after dropping it", rc);
    check(rc == -KOS_EPERM, "the just-dropped KOS_AUTH_PINMUX is now refused");

    if (failures != 0)
    {
        char msg[64];
        ksnprintf(msg, sizeof(msg), "[rootauth] FAIL (%d)\n", failures);
        emit(msg);
        return 1;
    }
    emit("[rootauth] PASS\n");
    return 0;
}
