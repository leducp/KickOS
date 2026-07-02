// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal out-of-tree KickOS application. Proves the dependency-inversion
// package: built entirely against the installed KickOS sim package.

#include <kickos/kos.hpp>

extern "C" void kickos_app_main(void) {
  kos::puts("[oot] hello from an out-of-tree KickOS app\n");
  // Returning drops the last non-idle thread; the kernel then halts cleanly.
}
