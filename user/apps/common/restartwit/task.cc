// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The witness, image 1: it reads its statics as the image holds them, writes them, and exits,
// so its restart reads them again from the image and never from the last start.

#include <kickos/kos.h>

#include <stdint.h>

namespace
{
    // Volatile, or a static only ever read is folded.
    volatile uint32_t g_seeded = 0x5EED1A71u;
    volatile uint32_t g_zero;
}

int main(int, char**)
{
    if (g_seeded != 0x5EED1A71u or g_zero != 0u)
    {
        kos::print("[restartwit] FAIL a start read data its image does not hold\n");
        kos_exit(2);
    }
    kos::print("[restartwit] pristine start\n");
    g_seeded = 0u;
    g_zero = 1u;
    kos_exit(1);
}
