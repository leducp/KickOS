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

// In-kernel IRQ-table size (sim placeholder; right-sized per chip at M1).
#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 32
#endif

namespace kickos
{
    // Tickless minimum-delta guard: never arm a one-shot timer closer than this
    // to "now", so we never program a compare that may already be in the past.
    constexpr uint64_t KICKOS_TIMER_MIN_DELTA_NS = 20_us;
}

#endif
