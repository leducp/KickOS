// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/kos.h>
#include <kickos/config/cap_width.h>

#include <cstdio>

static_assert(KICKOS_CAP_CHILD_WIDTH <= KICKOS_MAX_HANDLES,
              "the installed kickos/config/cap_width.h is the one the libraries were built "
              "with");

// The one include compiling the installed kickos/board_config.h and the chip's headers.
#include <kickos/config.h>
#ifdef KICKOS_EXPECT_MAX_THREADS
static_assert(KICKOS_MAX_THREADS == KICKOS_EXPECT_MAX_THREADS,
              "the installed provisioning is not the one the linked KickOS was built with");
#endif
#ifdef KICKOS_EXPECT_USER_STACK_SIZE
static_assert(KICKOS_USER_STACK_SIZE == KICKOS_EXPECT_USER_STACK_SIZE,
              "the installed provisioning is not the one the linked KickOS was built with");
#endif

// Must match tests/integration/check_oot_mcu_run.sh.
int main(int, char**)
{
    std::printf("[oot-mcu] hello from an out-of-tree bare-metal KickOS app\n");
    return 42;
}
