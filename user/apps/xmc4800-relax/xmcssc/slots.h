// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The two clients of the packaged xmcssc service (<kickos/apps/spi_slots.h>), on the channel's
// fixed rate and its hardware chip select.

#ifndef KICKOS_USER_APPS_XMC4800_RELAX_XMCSSC_SLOTS_H
#define KICKOS_USER_APPS_XMC4800_RELAX_XMCSSC_SLOTS_H

#include <kickos/apps/spi_slots.h>

namespace
{
    // hz 0 asks for the rate the channel was brought up at, the only one it takes.
    constexpr kickos::apps::spi_slots::Profile PROFILE = {"xmcslots", 0u, KOS_BUS_CS_HW};
}

#endif
