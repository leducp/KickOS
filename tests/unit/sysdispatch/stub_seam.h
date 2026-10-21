// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What stub_seam.cc records of the shutdown path and of the contexts it builds. Keep this header
// GTEST-FREE, for the reason kfixture.h states.

#ifndef KICKOS_TESTS_UNIT_SYSDISPATCH_STUB_SEAM_H
#define KICKOS_TESTS_UNIT_SYSDISPATCH_STUB_SEAM_H

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
    }
}

#endif
