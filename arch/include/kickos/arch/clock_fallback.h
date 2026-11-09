// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KICKOS_WITNESS_CLOCK_FALLBACK=1 (the CMake option) makes a chip's clock bring-up fail its PLL
// test, so the boot takes the branch a PLL that never locks takes. The root CMakeLists.txt names
// the chips that honour it.

#ifndef KICKOS_ARCH_CLOCK_FALLBACK_H
#define KICKOS_ARCH_CLOCK_FALLBACK_H

#ifndef KICKOS_WITNESS_CLOCK_FALLBACK
#define KICKOS_WITNESS_CLOCK_FALLBACK 0
#endif

#endif
