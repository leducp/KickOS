// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The host seam the doorbell templates call through: the interrupt mask as a flag, and every
// step the templates take as one character of a log.

#ifndef KICKOS_TESTS_UNIT_DOORBELL_SEAM_H
#define KICKOS_TESTS_UNIT_DOORBELL_SEAM_H

#include <string>

namespace kickos::doorbellfix
{
    extern bool g_masked;
    extern std::string g_log;
    // Called at each spin, or not at all while null.
    extern void (*g_on_spin)(void);

    void reset(void);
}

#endif
