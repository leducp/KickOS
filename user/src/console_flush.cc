// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys.h>
#include <kickos/sys/cap_index.h>

// A plain send is released when the driver takes it, so the first only starts the drain and the
// second is taken once the driver is back in its receive, past that drain.
int32_t kos_console_flush(uint32_t timeout_us)
{
    int32_t const first = kos_send_timed(KOS_CAP_STDOUT, "", 0, timeout_us);
    if (first < 0)
    {
        return first;
    }
    int32_t const second = kos_send_timed(KOS_CAP_STDOUT, "", 0, timeout_us);
    if (second < 0)
    {
        return second;
    }
    return 0;
}
