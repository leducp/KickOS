// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What stub_seam.cc records of the shutdown path and of the contexts it builds. Keep this header
// GTEST-FREE, for the reason kfixture.h states.

#ifndef KICKOS_TESTS_UNIT_SYSDISPATCH_STUB_SEAM_H
#define KICKOS_TESTS_UNIT_SYSDISPATCH_STUB_SEAM_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    namespace testfix
    {
        constexpr uint32_t REBOOT_NONE = 0xFFFFFFFFu;
        // The outcome arch_reboot was last asked for, which answers -KOS_ENOSYS as a chip with
        // no such entry does, and the console drains before it.
        extern uint32_t g_reboot_outcome;
        extern uint32_t g_console_drains;
        // The entry the last context built enters at, and where a backend translates the
        // second argument it was given (0 if none).
        extern uintptr_t g_context_entry;
        extern uintptr_t g_second_arg;
        constexpr uint64_t STUB_CPU_CLOCK_HZ = 64000000u;
        // What the dispatch handed the reply alone (it answers -KOS_ESRCH), and how often the
        // fused reply and receive was reached (it answers STUB_REPLY_RECV_RC).
        extern uint32_t g_replies;
        extern uint32_t g_reply_cap;
        extern uintptr_t g_reply_buf;
        extern size_t g_reply_len;
        extern uint32_t g_reply_recvs;
        constexpr int32_t STUB_REPLY_RECV_RC = 7;
        // The wait flag the last console write reached the kernel console with, and how many
        // reached it.
        extern bool g_kconsole_waits;
        extern uint32_t g_kconsole_writes;
        // How many console handovers a create began and finished.
        extern uint32_t g_handovers_begun;
        extern uint32_t g_handovers_done;
        // What console_owner_set_user runs once before it counts, a console driver's create
        // standing past its publish with the lock released; null for nothing.
        extern void (*g_handover_probe)(void);
        // The one device aperture arch_window_apertures answers, which holds no console window.
        constexpr uintptr_t STUB_DEVICE_BASE = 0x50000000u;
        constexpr size_t STUB_DEVICE_SIZE = 0x1000u;
    }
}

#endif
