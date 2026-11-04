// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The packaged drivers' STARTs the goldens' tables name. The init creates a driver's task and its
// entry thread runs the START, which the scripted kernel never runs: each START here refuses.

#ifndef KICKOS_TESTS_UNIT_INITWALK_DRIVERS_H
#define KICKOS_TESTS_UNIT_INITWALK_DRIVERS_H

struct kos_driver_instance;

extern "C"
{
    int xmcuartirq_console_start(struct kos_driver_instance* instance);
    int testdrv_start(struct kos_driver_instance* instance);
    int xmc_spi0_start(struct kos_driver_instance* instance);
}

#endif
