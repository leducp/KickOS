// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_USER_APPS_COMMON_INITDEMO_SINK_H
#define KICKOS_USER_APPS_COMMON_INITDEMO_SINK_H

#include <stdint.h>

#include <kickos/sys/atomic.h>

namespace initdemo
{
    // What the sink counts into: a block its creator reserves and shares through a window.
    struct SinkCount
    {
        kickos::Atomic<int32_t, kickos::Order::RELAXED> bytes;
    };

    constexpr uint32_t SINK_COUNT_BYTES = 64;
}

#endif
