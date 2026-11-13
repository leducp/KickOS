// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The app's image's kos_notify_wait, the link wrapping it in every image: the plain wait,
// root_depth.cc's scan being the init's.

#include <kickos/sys.h>

#include <stdint.h>

extern "C" int __real_kos_notify_wait(uint32_t mask, kos_monotonic_t until, uint32_t* out_bits);

extern "C" int __wrap_kos_notify_wait(uint32_t mask, kos_monotonic_t until, uint32_t* out_bits)
{
    return __real_kos_notify_wait(mask, until, out_bits);
}
