// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The region seam nocache_admission.cc sets: a whole fake arena, power-of-two and naturally
// aligned in 32-byte regions, apertures over the peripheral region and both bit-band aliases, one
// bus master, and no reserved row or bit-band alias until a case plants its own.

#ifndef KICKOS_TESTS_UNIT_GRANTNOCACHE_GRANT_SEAM_H
#define KICKOS_TESTS_UNIT_GRANTNOCACHE_GRANT_SEAM_H

#include <stddef.h>
#include <stdint.h>

#include "region_seam.h"

constexpr uintptr_t ARENA_BASE = 0x20010000u;
constexpr size_t ARENA_SIZE = 0x10000u;
constexpr size_t ARENA_REGION = 32u;

constexpr uintptr_t APERTURE_BASE = 0x40000000u;
constexpr size_t APERTURE_SIZE = 0x20000000u;
constexpr struct arch_reserved_block APERTURES[] = {
    {0x22000000u, 0x2000000u}, {APERTURE_BASE, APERTURE_SIZE}};

#endif
