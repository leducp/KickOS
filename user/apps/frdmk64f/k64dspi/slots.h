// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The two clients of the packaged k64dspi service (<kickos/apps/spi_slots.h>), with no chip select
// and no part on the bus.

#ifndef KICKOS_USER_APPS_FRDMK64F_K64DSPI_SLOTS_H
#define KICKOS_USER_APPS_FRDMK64F_K64DSPI_SLOTS_H

#include <kickos/apps/spi_slots.h>

namespace
{
    constexpr kickos::apps::spi_slots::Profile PROFILE = {"k64slots", 1000000u, KOS_BUS_CS_NONE};
}

#endif
