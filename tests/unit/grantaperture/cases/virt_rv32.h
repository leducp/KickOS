// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The virt_rv32 windows, from QEMU virt_memmap.

#ifndef KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_VIRT_RV32_H
#define KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_VIRT_RV32_H

#include "../aperture_case.h"

constexpr ApertureCase CASES[] = {
    {"TEST", 0x00100000u, 0x1000u, -KOS_EINVAL},
    {"CLINT", 0x02000000u, 0x10000u, -KOS_EINVAL},
    {"PLIC", 0x0C000000u, 0x1000u, -KOS_EINVAL},
    // A PCI device is a DMA master, and every BAR and config space reaches one.
    {"PCIE_PIO", 0x03000000u, 0x10000u, -KOS_EINVAL},
    {"PCIE_ECAM", 0x30000000u, 0x1000u, -KOS_EINVAL},
    {"PCIE_MMIO", 0x40000000u, 0x1000u, -KOS_EINVAL},
    {"RTC", 0x00101000u, 0x1000u, 0},
};

#endif
