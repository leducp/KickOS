// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The universal-default pin-map provider: no board pins to mux (the kernel muxed the
// console). Selected by KICKOS_BOARD_PINMAP on every board that ships no pin map.
// count = 0 is the "nothing to apply" signal, not a failure.

#include <kickos/sys/pinmap.h>

extern "C"
{
    struct kos_board_pinmap const kickos_board_pinmap = { nullptr, 0 };
}
