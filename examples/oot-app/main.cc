// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal out-of-tree KickOS application. Proves the dependency-inversion
// package: built entirely against the installed KickOS sim package.

#include <kickos/kos.h>

// The only gate that compiles the INSTALLED config/cap_width.h: nothing on the kos.h path
// reaches it, so its install() rule could regress unnoticed.
#include <kickos/config/cap_width.h>
static_assert(KICKOS_CAP_CHILD_WIDTH <= KICKOS_MAX_HANDLES,
              "the installed kickos/config/cap_width.h is the one the libraries were built "
              "with");

// A plain, OS-agnostic entry: the KickOS package renames it to the kernel entry.
int main(int, char**)
{
    kos::print("[oot] hello from an out-of-tree KickOS app\n");
    return 0; // single-shot: returning exits the sim with this status
}
