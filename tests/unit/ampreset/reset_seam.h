// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_TESTS_UNIT_AMPRESET_RESET_SEAM_H
#define KICKOS_TESTS_UNIT_AMPRESET_RESET_SEAM_H

#include <stdint.h>

namespace kickos
{
    namespace resetfix
    {
        constexpr uint32_t REBOOT_NONE = 0xFFFFFFFFu;
        extern uint32_t g_authority;
        extern uint32_t g_reboot_outcome;
    }
}

#endif
