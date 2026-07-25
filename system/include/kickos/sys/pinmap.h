// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-board pin-mux map. The default init applies it BEFORE the service
// list: it is the DAG middle of the clock->pinmux->gpio bring-up chain. Each
// entry points a pin at a raw chip function code (`func` is the chip's PC/PCR
// encoding, opaque to the map). Lives in the kickos_system library; keep it
// dependency-free (shared verbatim by the init body and every per-board provider TU).

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
// the KICKOS_BOARD_PINMAP CMake target (default kickos_pinmap_none, count = 0), like
// kickos_board_services. See system/init/pinmap_none.cc and a per-board provider such
// as system/init/pinmap_xmc4800relax.cc.
extern struct kos_board_pinmap const kickos_board_pinmap;

#ifdef __cplusplus
}
#endif

#endif
