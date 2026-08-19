// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// lib/libc/string.cc compiled under prefixed names. Linking its real names into a host
// executable would override the host libc's for GoogleTest and the C++ runtime too, so a
// deliberately broken implementation would crash the harness instead of failing a case.
// KICKOS_ARCH_SIM is forced to 0 here: the sim build defines these away, and this gate
// exists to test the definitions the TARGETS compile.

#undef KICKOS_ARCH_SIM
#define KICKOS_ARCH_SIM 0

#define memcpy kos_ut_memcpy
#define memset kos_ut_memset
#define memmove kos_ut_memmove
#define memcmp kos_ut_memcmp
#define strlen kos_ut_strlen
#define strnlen kos_ut_strnlen

#include "../../../lib/libc/string.cc"
