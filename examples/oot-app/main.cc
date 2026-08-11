// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal out-of-tree KickOS application. Proves the dependency-inversion
// package: built entirely against the installed KickOS sim package.

#include <kickos/kos.h>

// kos.h already reaches config/cap_width.h (kos.h -> sys.h -> sys/abi.h -> sys/cap_index.h);
// named here because the assertion below is what reads it, not because the include is
// otherwise missing.
#include <kickos/config/cap_width.h>
static_assert(KICKOS_CAP_CHILD_WIDTH <= KICKOS_MAX_HANDLES,
              "the installed kickos/config/cap_width.h is the one the libraries were built "
              "with");

// The provisioning half of the same fact, and the only thing that compiles the installed
// kickos/board_config.h and the chip's include directory. Nothing on the kos.h path pulls
// a kernel config header, so both install() rules could be deleted with this gate still
// green. KICKOS_EXPECT_* are the values the KickOS build this links against resolved,
// passed in by tests/integration/check_oot_export.sh; without board_config.h the knobs fall back to
// config/system.h's fleet defaults and the libraries are sized differently, which nothing
// would report until a spawn failed on the target.
#include <kickos/config.h>
#ifdef KICKOS_EXPECT_MAX_THREADS
static_assert(KICKOS_MAX_THREADS == KICKOS_EXPECT_MAX_THREADS,
              "the installed provisioning is not the one the linked KickOS was built with");
#endif
#ifdef KICKOS_EXPECT_USER_STACK_SIZE
static_assert(KICKOS_USER_STACK_SIZE == KICKOS_EXPECT_USER_STACK_SIZE,
              "the installed provisioning is not the one the linked KickOS was built with");
#endif

// A plain, OS-agnostic entry: the KickOS package renames it to the kernel entry.
int main(int, char**)
{
    kos::print("[oot] hello from an out-of-tree KickOS app\n");
    return 0; // single-shot: returning exits the sim with this status
}
