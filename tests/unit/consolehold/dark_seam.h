// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What dark_seam.cc records beside the real console.

#ifndef KICKOS_TESTS_UNIT_CONSOLEHOLD_DARK_SEAM_H
#define KICKOS_TESTS_UNIT_CONSOLEHOLD_DARK_SEAM_H

#include <stdint.h>

#include <string>

namespace darkseam
{
    // Bytes the device took, in order.
    extern std::string g_wire;
    // arch_console_reclaim calls.
    extern uint32_t g_reclaims;
    // Whether the console's register window is free.
    extern bool g_window_free;

    void reset();
}

#endif
