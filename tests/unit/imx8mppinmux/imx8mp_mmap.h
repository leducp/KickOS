// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The i.MX 8M Plus base <pinmux.h> names (IMX8MPRM rev 3, Table 2-3).

#ifndef KICKOS_TESTS_UNIT_IMX8MPPINMUX_IMX8MP_MMAP_H
#define KICKOS_TESTS_UNIT_IMX8MPPINMUX_IMX8MP_MMAP_H

#include <stdint.h>

namespace kickos::imx8mp::mmap
{
    constexpr uintptr_t IOMUXC_BASE = 0x30330000u;
}

#endif
