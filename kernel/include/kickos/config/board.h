// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Board / chip-derived configuration. These are hardware facts, not app knobs:
// MAX_IRQ is sized to the chip's NVIC line count, MIN_DELTA to the timer resolution.

#ifndef KICKOS_CONFIG_BOARD_H
#define KICKOS_CONFIG_BOARD_H

#include <stdint.h>

#include <kickos/units.h>

// The provisioning knobs (config/system.h) come from board_config.h, generated into the
// build tree and installed for out-of-tree consumers. A standalone build has none and
// falls through to the defaults. A CMake -D still wins: those defines are #ifndef-guarded.
#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif

// The chip's constants. They must stay out of the generated board_config.h, which would
// otherwise shadow them.
#if defined(__has_include) && __has_include(<kickos/chip_limits.h>)
#include <kickos/chip_limits.h>
#endif

// The sim's value, not a fleet-wide fallback. A real chip whose chip_limits.h is off the
// include path must fail here rather than silently size its IRQ table to 32.
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
