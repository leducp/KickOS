// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel clock held against a clock outside the chip: three marks a requested sleep apart,
// which tests/integration/check_wallclock.sh times between the second and third by the capture
// host's own arrival stamps. A USB device console loses the first.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

namespace
{
    constexpr uint64_t SLEEP_NS = 5000000000ull;
}

int main(int, char**)
{
    char line[160];
    ksnprintf(line, sizeof(line), "[wallclock] mark 0, sleeping %llu ns\n",
              static_cast<unsigned long long>(SLEEP_NS));
    kos::print(line);
    uint64_t const t0 = kos::clock_now();
    kos::sleep_ns(SLEEP_NS);
    uint64_t const t1 = kos::clock_now();
    ksnprintf(line, sizeof(line),
              "[wallclock] mark 1, the kernel clock advanced %llu ns, sleeping %llu ns\n",
              static_cast<unsigned long long>(t1 - t0), static_cast<unsigned long long>(SLEEP_NS));
    kos::print(line);
    kos::sleep_ns(SLEEP_NS);
    uint64_t const t2 = kos::clock_now();
    ksnprintf(line, sizeof(line), "[wallclock] mark 2, the kernel clock advanced %llu ns\n",
              static_cast<unsigned long long>(t2 - t1));
    kos::print(line);
    kos::print("[wallclock] done\n");
    return 0;
}
