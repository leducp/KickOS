// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Board / chip-derived configuration. These are hardware facts, not app knobs;
// at M1/M2 they leave the global config entirely for the board layer (MAX_IRQ
// sized to the chip's NVIC line count, MIN_DELTA to the timer resolution). The
// values here are the sim placeholders.

#ifndef KICKOS_CONFIG_BOARD_H
#define KICKOS_CONFIG_BOARD_H

#include <stdint.h>

#include <kickos/units.h>

// The provisioning KNOBS (config/system.h) live in board_config.h, which on a board
// configured from Kconfig is generated into the build tree. CMake adds the directory
// to the include path and installs it for out-of-tree consumers. A plain
// sim/standalone build has none and falls through to the defaults. A CMake -D still
// overrides, because those defines are #ifndef-guarded.
#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif

// The chip's CONSTANTS, which are a different kind of thing and so a different header:
// nothing configures them, no option depends on them, and they are defined
// unconditionally. Keeping them out of board_config.h is what lets that header be
// generated from the configuration without shadowing them.
#if defined(__has_include) && __has_include(<kickos/chip_limits.h>)
#include <kickos/chip_limits.h>
#endif

// The sim has no chip and therefore no chip_limits.h. This is its value, not a
// fleet-wide fallback: every real chip defines the macro unconditionally, so a chip
// whose header is missing from the include path fails here rather than silently
// sizing its IRQ table to 32.
#ifndef KICKOS_MAX_IRQ
#if defined(KICKOS_ARCH_SIM) && KICKOS_ARCH_SIM
#define KICKOS_MAX_IRQ 32
#else
#error "no kickos/chip_limits.h on the include path: the chip's KICKOS_MAX_IRQ is missing"
#endif
#endif

namespace kickos
{
    // Tickless minimum-delta guard: never arm a one-shot timer closer than this
    // to "now", so we never program a compare that may already be in the past.
    constexpr uint64_t KICKOS_TIMER_MIN_DELTA_NS = 20_us;
}

#endif
