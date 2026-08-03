// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-board pin-mux map, applied by the default init BEFORE the service list. `func`
// is the chip's raw PC/PCR function encoding, opaque to the map. Lives in the
// kickos_system library; keep it dependency-free (shared verbatim by the init body and
// every per-board provider TU).

#ifndef KICKOS_SYS_PINMAP_H
#define KICKOS_SYS_PINMAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

struct kos_pinmux_entry
{
    uint16_t port;
    uint16_t pin;
    uint32_t func;
};

struct kos_board_pinmap
{
    struct kos_pinmux_entry const* entries;
    uint32_t count;
};

// The selected board's pin map. EXACTLY ONE definition links per image, chosen by
// the KICKOS_BOARD_PINMAP CMake target (default kickos_pinmap_none, count = 0).
extern struct kos_board_pinmap const kickos_board_pinmap;

#ifdef __cplusplus
}
#endif

#endif
