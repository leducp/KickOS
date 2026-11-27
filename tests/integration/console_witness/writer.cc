// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The console restart witness's writer: LINES numbered lines on stdout, PACE apart, each one
// write, then its return ends the system. A line the console refuses, its driver gone, is lost:
// the writer clears the stream's error and goes on. Its status: 0 where at least one line was
// refused and every refusal was ECONNREFUSED, 1 where none was, 2 where one answered otherwise.

#include <kickos/sys.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

namespace
{
    constexpr int LINES = 30;
    constexpr int64_t PACE_MS = 20;
}

int main(int, char**)
{
    int refused = 0;
    int other = 0;
    for (int k = 1; k <= LINES; k++)
    {
        errno = 0;
        if (printf("conwit: line %d out\n", k) < 0)
        {
            refused++;
            if (errno != ECONNREFUSED)
            {
                other++;
            }
            clearerr(stdout);
        }
        kos_sleep_for(kos_ms(PACE_MS));
    }
    if (other != 0)
    {
        return 2;
    }
    if (refused == 0)
    {
        return 1;
    }
    return 0;
}
