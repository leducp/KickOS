// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_USER_APPS_COMMON_FAULTSURVIVE_FS_H
#define KICKOS_USER_APPS_COMMON_FAULTSURVIVE_FS_H

#include <stdint.h>

// Mode 4: the size clears KICKOS_MIN_STACK_SIZE, so the worker is a legitimate thread. Under an
// enforcing MPU the stack is one region, so PMP NAPOT wants the base aligned to its size; main
// checks that it got it.
constexpr uint32_t FS_STACK_SIZE = 2048;
constexpr uint32_t FS_BAND_SIZE = 512;
constexpr uint32_t FS_BAND_POISON = 0x5AFEBA5Eu;

// Modes 2 and 7: the worker's stack and the block its SP is aimed at, each this size.
constexpr uint32_t FS_OFF_STACK_SIZE = 2048;

// Mode 10: the software traps the peer is spawned for, one per spawn, its index the argument.
// brk is LAST: on a backend that panics on it, every entry before it still reaches the wire.
constexpr uint32_t FS_SWTRAP_COUNT = 9;

#endif
