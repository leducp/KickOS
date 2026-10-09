// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arena under the real system-call layer at the region posture; region_seam.cc says what
// it answers. Keep this header GTEST-FREE, for the reason kfixture.h states.

#ifndef KICKOS_TESTS_UNIT_SYSDISPATCH_REGION_SEAM_H
#define KICKOS_TESTS_UNIT_SYSDISPATCH_REGION_SEAM_H

#include <kickos/config.h>

#include <stddef.h>

namespace kickos
{
    namespace testfix
    {
        constexpr size_t ARENA_BYTES = 2u * KICKOS_USER_STACK_SIZE;
        constexpr size_t REGION_MIN = 256u;
        extern unsigned char g_arena[ARENA_BYTES];
        extern size_t g_arena_used;
    }
}

#endif
