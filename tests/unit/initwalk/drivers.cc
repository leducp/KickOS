// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "drivers.h"

extern "C"
{
    int xmcuartirq_console_start(struct kos_driver_instance*)
    {
        return -1;
    }

    int testdrv_start(struct kos_driver_instance*)
    {
        return -1;
    }

    int xmc_spi0_start(struct kos_driver_instance*)
    {
        return -1;
    }
}
