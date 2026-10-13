// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What dark_seam.cc records beside the real console.

#ifndef KICKOS_TESTS_UNIT_CONSOLEHOLD_DARK_SEAM_H
#define KICKOS_TESTS_UNIT_CONSOLEHOLD_DARK_SEAM_H

#include <stdint.h>

#include <string>

namespace kickos
{
    struct Endpoint;
    struct Task;
    struct Thread;
}

namespace darkseam
{
    // Bytes the device took, in order.
    extern std::string g_wire;
    // arch_console_reclaim calls.
    extern uint32_t g_reclaims;
    // Whether the console's register window is free.
    extern bool g_window_free;

    void reset();

    // KOS_SYS_CONSOLE_PUBLISH's console sequence, with no writer in flight to drain: `publisher`
    // publishes `ep` for `served`. False, with nothing handed over, where the publish is refused.
    bool publish(kickos::Thread* publisher, kickos::Endpoint* ep, kickos::Task* served);
}

#endif
