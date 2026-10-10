// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the arch seam in nocache_admission.cc answers: a whole fake arena, power-of-two and
// naturally aligned in 32-byte regions, and chip tables empty with no bit-band alias until a
// case plants its own.

#ifndef KICKOS_TESTS_UNIT_GRANTNOCACHE_GRANT_SEAM_H
#define KICKOS_TESTS_UNIT_GRANTNOCACHE_GRANT_SEAM_H

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

constexpr uintptr_t ARENA_BASE = 0x20010000u;
constexpr size_t ARENA_SIZE = 0x10000u;
constexpr size_t ARENA_REGION = 32u;

extern struct arch_reserved_span g_reserved;
extern int g_bitband;

#endif
