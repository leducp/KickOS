// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal out-of-tree KickOS application. Proves the dependency-inversion
// package: built entirely against the installed KickOS sim package.

#include <kickos/kos.h>

// A plain, OS-agnostic entry: the KickOS package renames it to the kernel entry.
int main(int, char**)
{
    kos::print("[oot] hello from an out-of-tree KickOS app\n");
    return 0; // single-shot: returning exits the sim with this status
}
