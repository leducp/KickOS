// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// lib/libc/string.cc compiled a second time under the names kickos/kruntime.h declares,
// as tests/unit/kstring/kstring_shim.cc already does for the host gate. KICKOS_ARCH_SIM is
// forced to 0 because the sim build defines the ordinary names away in favour of the host
// libc's, and these names are private, so no definition here can preempt one of those.

#undef KICKOS_ARCH_SIM
#define KICKOS_ARCH_SIM 0

#define memcpy kmemcpy
#define memset kmemset
#define memmove kmemmove
#define memcmp kmemcmp
#define strlen kstrlen
#define strnlen kstrnlen

#include "../../lib/libc/string.cc"
