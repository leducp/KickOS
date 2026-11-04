// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The slot witness's probe, a program of its own holding no window: main creates it once the
// holder has ended, and its read of MCR must fault.

#include "witness.h"

using namespace k64dspi_witness;

int main(int, char**)
{
    probe(reinterpret_cast<void*>(mmap::DSPI0_BASE));
    return 0;
}
