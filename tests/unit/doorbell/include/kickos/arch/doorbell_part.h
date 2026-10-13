// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The host part the doorbell templates are instantiated over. Its spin is recorded.

#ifndef KICKOS_ARCH_DOORBELL_PART_H
#define KICKOS_ARCH_DOORBELL_PART_H

#include <kickos/arch/arch.h>

#define KICKOS_DOORBELL_LINE 4u
#define KICKOS_DOORBELL_TAG "host"

namespace kickos::doorbell
{
    void host_note(char step);

    inline void part_spin(void)
    {
        host_note('S');
    }
}

#endif
