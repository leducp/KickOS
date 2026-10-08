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

#include <kickos/apps/arm_count.h>

namespace
{
    // Out-of-range on every chip's {port,pin} encoding, so a real backend rejects it
    // before touching a mux register.
    constexpr uint32_t BAD_PORT = 0xFFFFu;
    constexpr uint32_t BAD_PIN = 0xFFFFu;

    kickos::apps::ArmCount harness("rootauth");
}

int main(int, char**)
{
    // -KOS_EPERM here means the declaration did not reach the spawn. Any other rc is the
    // backend answering past the authority gate.
    int rc = kos_pinmux_set(BAD_PORT, BAD_PIN, 0);
    harness.note_rc("pinmux_set (declared bit)", rc);
    harness.check(rc != -KOS_EPERM, "declared KOS_AUTH_PINMUX reached the spawn");

    // The authority gate precedes the cap lookup, so the handle must stay bogus: a
    // -KOS_EBADF here would mean the gate let the call through to the lookup.
    rc = kos_console_publish(-1, KOS_TASK_NONE);
    harness.note_rc("console_publish (undeclared bit)", rc);
    harness.check(rc == -KOS_EPERM, "undeclared KOS_AUTH_CONSOLE was not granted");

    // Must precede the narrow below, which drops KOS_AUTH_MEMORY. Leaks one block of
    // arch_ram_region_size(1); kos_ram_alloc never frees.
    void* mem = kos_ram_alloc(1);
    harness.check(mem != nullptr, "declared KOS_AUTH_MEMORY reached kos_ram_alloc");

    rc = kos_cap_narrow(KOS_CAP_AUTHORITY, KOS_AUTH_SYSTEM);
    harness.note_rc("cap_narrow to KOS_AUTH_SYSTEM", rc);
    harness.check(rc == 0, "main narrowed its own cap further");

    // Separates a narrow that took effect from one that returned 0 and changed nothing.
    rc = kos_pinmux_set(BAD_PORT, BAD_PIN, 0);
    harness.note_rc("pinmux_set after dropping it", rc);
    harness.check(rc == -KOS_EPERM, "the just-dropped KOS_AUTH_PINMUX is now refused");

    return harness.verdict();
}
