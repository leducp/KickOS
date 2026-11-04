// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kos_panic wire gate. One source, six images selected by KICKOS_PANICGATE_CASE, because
// only one panic can be observed per run:
//   1  a readable message must reach the wire after the kernel's trusted banner
//   2  a null pointer must reach the documented fallback text
//   3  a pointer in no region this thread holds and outside every linker-defined extent
//      must reach the SAME fallback text, and must not fault the kernel: the copy runs
//      privileged, so admitting it would take the fault inside kaccess_from_user
//   4  a message longer than the kernel buffer must appear truncated WITH A VISIBLE
//      marker, and with the tail absent
//   5  control bytes in the message must be replaced in place, so the panic cannot
//      continue onto lines that read as kernel output
//   6  a thread holding no authority panics: its task ends with KOS_EXIT_PANIC, its
//      message reaches the wire as a record, and main runs on to read that status
//
// Case 3 discriminates only while main is UNPRIVILEGED: a privileged caller passes
// user_readable_ok wholesale.

#include <kickos/chip_limits.h> // KICKOS_CHIP_ONE_TASK
#include <kickos/kos.h>
#include <kickos/sys.h>

#ifndef KICKOS_PANICGATE_CASE
#error "KICKOS_PANICGATE_CASE must be 1, 2, 3, 4, 5 or 6"
#endif

namespace
{
    // Mapped on no board in the fleet: below every RAM origin the ports use
    // (0x20000000 armv7m, 0x38000000 an505, 0x40800000 esp32c6, 0x80000000 virt) and
    // above every flash/ROM extent, so a read here BUSFAULTS instead of returning junk.
    constexpr uintptr_t WILD = 0xCCCCCCC0u;

    // The kernel buffer is 64 including the terminator, so 63 bytes survive, and the
    // last 3 of those carry the truncation marker: 60 kept bytes then "...". "WXYCUTME"
    // must not reach the wire: WXY because the marker overwrote it, CUTME because it was
    // past the buffer.
    constexpr char const* LONG_MSG =
        "[panicgate] abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYCUTME";
    static_assert(sizeof("[panicgate] abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV")
                      == 61,
                  "the kept prefix is the kernel buffer's 63 bytes less the 3-byte marker");

    // LF, TAB, CR and DEL inside an otherwise printable message. Split from the
    // neighbouring literals so the \x7f escape cannot absorb a following hex digit.
    constexpr char const* CTL_MSG = "[panicgate] ctl" "\n\t\r\x7f" " end";

#if KICKOS_PANICGATE_CASE == 6
    // Main's own end status: 0 once the panic ended the task alone, and the system with it.
    int task_case()
    {
        kos::print("[panicgate] case 6: a task without the system authority panics\n");
#if KICKOS_CHIP_ONE_TASK
        kos::print("[panicgate] skip - the panicking task needs a task of its own and this board "
                   "holds one task\n");
        return 0;
#else
        kos_program_t prog = KOS_CAP_NONE;
        if (kos_program_find("panicgate_peer", &prog) != 0)
        {
            kos::print("[panicgate] ERROR: no peer program\n");
            return 1;
        }
        kos_task_params params = {};
        kos_task_t t = KOS_TASK_NONE;
        int const rc = kos_task_create(prog, &params, &t);
        (void)kos_handle_close(prog);
        if (rc != 0)
        {
            kos::print("[panicgate] ERROR: kos_task_create refused\n");
            return 1;
        }
        int status = -1;
        int state = 0;
        for (int i = 0; i < 2000; i++)
        {
            state = kos_task_state(t, &status);
            if (state >= 0 and (state & KOS_TASK_DEAD) != 0)
            {
                break;
            }
            kos_sleep_ns(1000000u);
        }
        if (state < 0 or (state & KOS_TASK_DEAD) == 0)
        {
            kos::print("[panicgate] ERROR: the task never died\n");
            return 1;
        }
        if (status != KOS_EXIT_PANIC)
        {
            kos::print("[panicgate] ERROR: the task ended with another status\n");
            return 1;
        }
        (void)kos_task_slay(t, 0);
        kos::print("[panicgate] the task ended with KOS_EXIT_PANIC, the system runs on\n");
        return 0;
#endif
    }
#endif
}

int main(int, char**)
{
#if KICKOS_PANICGATE_CASE == 1
    kos::print("[panicgate] case 1: readable message\n");
    kos_panic("[panicgate] message on the wire");
#elif KICKOS_PANICGATE_CASE == 2
    kos::print("[panicgate] case 2: null message\n");
    kos_panic(nullptr);
#elif KICKOS_PANICGATE_CASE == 3
    kos::print("[panicgate] case 3: unreadable message\n");
    kos_panic(reinterpret_cast<char const*>(WILD));
#elif KICKOS_PANICGATE_CASE == 4
    kos::print("[panicgate] case 4: oversized message\n");
    kos_panic(LONG_MSG);
#elif KICKOS_PANICGATE_CASE == 5
    kos::print("[panicgate] case 5: control bytes in the message\n");
    kos_panic(CTL_MSG);
#else
    return task_case();
#endif
    kos::print("[panicgate] ERROR: kos_panic returned\n");
    return 1;
}
