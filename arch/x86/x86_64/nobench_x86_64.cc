// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The switch accumulator's two symbols, for the kernel-free images. Under KICKOS_BENCH
// switch.S brackets the swap and reaches kernel/bench/bench.cc for both of these, and the
// X3, X4 and X5 images carry the arch archive with no kernel behind it.

#include <kickos/board_config.h>

#include <stdint.h>

#if defined(KICKOS_BENCH) && KICKOS_BENCH
extern "C"
{

uint32_t g_bench_sw_start[KICKOS_NUM_CORES] = {};

void kickos_bench_switch_done(uint32_t delta)
{
    (void)delta;
}

}
#endif
