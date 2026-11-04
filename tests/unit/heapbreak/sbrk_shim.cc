// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// user/src/newlib_sbrk.cc, and a reset of its break and its seat to the untouched window, so
// each arm starts as an image's first allocation does.

#include "../../../user/src/newlib_sbrk.cc"

extern "C" void kos_ut_heap_reset(void)
{
    s_brk = _kickos_heap_start;
    s_seated = false;
}
